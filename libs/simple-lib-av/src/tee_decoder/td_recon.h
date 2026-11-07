/* TEE Decoder building block - made by tools/make_baustein.sh from common/recon.h; do not edit here: change the
   project's file and run the script again. */
/* H.264 High Profile picture reconstruction (8-bit 4:2:0, frame coding, one slice per picture): from
   the CABAC parser's macroblock records (cabac_slice.h) to decoded, deblocked pixels.

   Two passes per picture, as the SPU runs them:
     1. reconstruction: motion vectors (prediction, P_Skip, spatial direct), motion compensation
        (quarter-pel luma, 1/8 chroma, default / explicit / implicit weighting), intra prediction,
        dequantisation + inverse transforms; writes unfiltered pixels, per-MB deblocking data (bS, QP)
        and the picture's motion field (for later pictures' direct prediction)
     2. deblocking, macroblock by macroblock in address order, from the stored bS/QP data

   The pixel math is in recon_kernels.h, shared by the Mac reference and the SPU program. */
#ifndef RECON_H
#define RECON_H

#include <stddef.h>
#include <stdint.h>

#define RECON_BORDER    64          /* luma border around every picture (chroma: half) */
#define RECON_MAX_REFS  32

/* per 8x8 block of a decoded picture (its outer-corner 4x4 block, direct_8x8_inference) */
typedef struct {
	int16_t mv[2][2];
	int8_t ref[2];                  /* ref_idx in that picture's lists, -1 = list unused (intra: both -1) */
	int16_t refid[2];               /* id of the picture each list refers to (temporal direct) */
	int16_t pad;
} recon_col;                        /* 16 bytes */

typedef struct {
	uint8_t *y, *u, *v;             /* sample (0, 0), inside the border */
	int stride_y, stride_c;
	int poc, long_term, id;         /* id: unique per decoded picture (deblocking, temporal direct) */
	recon_col *col;                 /* mb_count * 4 */
} recon_pic;

/* deblocking data of one macroblock, written by pass 1 */
typedef struct {
	uint8_t bs[2][4][4];            /* [0 vertical edges x = 0,4,8,12 | 1 horizontal edges][edge][4-sample segment] */
	int8_t qp, qp_left, qp_top;     /* QP_Y as the filter sees it (I_PCM: 0) */
	uint8_t pad[13];
} recon_dbk;                        /* 48 bytes: bs[0] and bs[1] each a quadword (SIMD) */

typedef struct {
	int slice_type, mb_w, mb_h;
	int num_ref[2];
	recon_pic *ref[2][RECON_MAX_REFS];
	int cur_poc;
	int direct_spatial, direct_8x8_inference, transform_8x8_mode;
	int wp_mode;                    /* 0 default, 1 explicit (pred_weight_table), 2 implicit */
	int luma_log2, chroma_log2;
	int lw[2][RECON_MAX_REFS], lo[2][RECON_MAX_REFS];
	int cw[2][RECON_MAX_REFS][2], co[2][RECON_MAX_REFS][2];
	int cqp_offset[2];              /* chroma_qp_index_offset, second_chroma_qp_index_offset */
	int disable_deblock, alpha_offset, beta_offset;
} recon_params;

/* time spent per part (nanoseconds on the Mac, decrementer ticks on the SPU):
   RT_MV       stage A: record walk, motion vectors / intra modes, the MC plan
   RT_MC       motion compensation incl. weighting      RT_DMAWAIT  waiting for reference fetches
   RT_INTRA    intra prediction incl. its residual      RT_RESID    inter residual (dequant + IDCT)
   RT_STORE    neighbour loads, MB stores               RT_BS       deblocking strength, motion field
   RT_DEBLOCK  pass 2                                   RT_BORDER   edge extension of reference pictures
   RT_OTHER    the rest (record input)
   RT_REFWAIT  (TEE Decoder) waiting for the references' progress (other SPEs still making them)
   RT_ROWDMA   (TEE Decoder) the deblocking's DMA in and out, row by row, and the progress word */
enum { RT_MV, RT_MC, RT_DMAWAIT, RT_INTRA, RT_RESID, RT_STORE, RT_BS, RT_DEBLOCK, RT_BORDER, RT_OTHER,
       RT_REFWAIT, RT_ROWDMA,
       /* inside RT_MV (not added to the total): record walk + MB kind, motion vectors, MC plan, DMA list */
       RT_A_WALK, RT_A_MOTION, RT_A_PLAN, RT_A_FETCH,
       /* (TEE Decoder 1.2) inside RT_A_MOTION by macroblock kind - P_Skip, B_Skip / B_Direct_16x16, 16x16 /
          16x8 / 8x16, 8x8 - as time, then counts; and the colocated motion field's DMA waits (time, count) */
       RT_M_PSKIP, RT_M_DIRECT, RT_M_INTER, RT_M_SUB8, RT_N_PSKIP, RT_N_DIRECT, RT_N_INTER, RT_N_SUB8, RT_COLWAIT, RT_N_COLWAIT,
       RT_PARTS };

enum {
	RECON_OK = 0, RECON_ERR_RECORD = 1, RECON_ERR_REF = 2, RECON_ERR_TEMPORAL = 3
};

/* pass 1 + pass 2 for one picture; rec/nvalues: the parser's output stream. dbk: mb_count entries. */
int recon_picture(const recon_params *p, const int16_t *rec, size_t nvalues, recon_pic *out, recon_dbk *dbk,
                  int deblock, uint64_t *times);

/* replicates the picture's edge samples into its border (needed before it is used as a reference) */
void recon_extend_borders(recon_pic *pic, int width, int height);

#endif

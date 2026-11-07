/* TEE Decoder building block - made by tools/make_baustein.sh from common/cabac_slice.h; do not edit here: change the
   project's file and run the script again. */
/* CABAC slice-data parser for H.264 High Profile (8-bit 4:2:0, progressive): entropy decoding and
   macroblock-layer syntax, no pixel reconstruction. Shared by the Mac tools and the SPU program.

   The parser writes what a reconstruction stage would need as a stream of int16 values (all fields
   are int16, so the stream hashes the same on a little-endian Mac and the big-endian Cell):

     per macroblock: a 32-value header (see MBH_*), then
       I_NxN:      nothing extra (the 16 raw intra modes are packed into the header)
       I_PCM:      384 samples
       inter:      n_mvd pairs (x, y), padded to a multiple of 8 values
       residual:   every coded block in decoding order, dense, coefficients in scan order:
                   luma DC 16, luma 4x4 16 (AC blocks: index 0 unused), luma 8x8 64,
                   chroma DC 8 (4 used), chroma AC 16 (index 0 unused)

   Build variants (one translation unit each):
     CABAC_ENGINE_EXACT  - spec arithmetic decoder, bit by bit (the reference)
     default             - fast decoder (32-bit scaled offset, 16-bit refills), Mac and SPU
*/
#ifndef CABAC_SLICE_H
#define CABAC_SLICE_H

#include <stdint.h>

#define CABAC_MAX_MB_W    128          /* up to 2048 pixels wide */

/* everything the slice data needs from the headers; filled by the PPU / Mac side */
typedef struct {
	int32_t slice_type;                /* SLICE_P / SLICE_B / SLICE_I (h264_hdr.h) */
	int32_t slice_qp, cabac_init_idc;
	int32_t num_ref_idx_l0, num_ref_idx_l1;   /* active counts */
	int32_t transform_8x8_mode, direct_8x8_inference;
	int32_t mb_width, mb_height, first_mb;
	int32_t data_bytes;                /* slice data bytes (from the first CABAC byte to the RBSP's end) */
	int32_t stop_bit;                  /* position of rbsp_stop_one_bit, in bits from the slice data start */
	int32_t pad[4];
} cabac_slice_params;

/* bin classes for the statistics (reference engine only) */
enum {
	BC_SKIP, BC_MBTYPE, BC_SUBTYPE, BC_REF, BC_MVD, BC_CBP, BC_QP, BC_INTRA, BC_T8,
	BC_CBF, BC_SIG, BC_LAST, BC_LEVEL, BC_END, BC_CLASSES
};

typedef struct {
	uint32_t mbs, skip, intra, pcm, i16, inxn, t8, inter;
	uint32_t bins_decision, bins_bypass, bins_terminate;
	uint32_t consumed_bits;            /* bits of slice data the decoder read */
	int32_t  end_ok;                   /* 1: end_of_slice_flag after the last MB, only trailing bits left */
	int32_t  error;                    /* 0 = ok */
	uint32_t out_values;               /* int16 values written */
	uint32_t coeffs;                   /* nonzero coefficients */
	uint32_t class_bins[BC_CLASSES];
	int32_t  end_gap;                  /* stop bit position + 1 - consumed bits (0..7 expected) */
	uint32_t pad[1];
} cabac_stats;

enum {
	CABAC_OK = 0,
	CABAC_ERR_MBTYPE = 1, CABAC_ERR_REF = 2, CABAC_ERR_QP = 3, CABAC_ERR_OVERRUN = 4,
	CABAC_ERR_PARAMS = 5, CABAC_ERR_MVD = 6, CABAC_ERR_LEVEL = 7
};

/* header layout (int16 values) */
enum {
	MBH_ADDR, MBH_TYPE, MBH_CBP, MBH_QP, MBH_FLAGS, MBH_CPRED,
	MBH_SUB0, MBH_SUB1, MBH_SUB2, MBH_SUB3,
	MBH_REF0, MBH_REF1, MBH_REF2, MBH_REF3,      /* ref_idx_l0 | ref_idx_l1 << 8 per 8x8, 0xFF = none */
	MBH_NMVD, MBH_CBF_LUMA, MBH_CBF_CHROMA,      /* luma: raster bit y*4+x; chroma: bit0 luma DC, 1-2 DC, 3-10 AC */
	MBH_IPRED0,                                  /* 4 values: 16 x 4 bits (8 = prev flag, else rem) */
	MBH_LEN = MBH_IPRED0 + 4,                    /* values in this record, header included */
	MBH_SIZE = 32
};
#define MBF_OUT_T8    1
#define MBF_OUT_PCM   2
#define MBF_OUT_SKIP  4
#define MBT_SKIP     (-1)

/* The output sink: values go to [wp, end); when fewer than CABAC_MB_ROOM are left before a macroblock,
   flush() is called and must make room (the SPU sends the buffer by DMA, the Mac hashes it). */
#define CABAC_MB_ROOM 1024
typedef struct cabac_out cabac_out;
struct cabac_out {
	int16_t *wp, *end;
	void (*flush)(cabac_out *o);
	void *user;
};

/* The bitstream source for the fast engine: 16-bit units at even byte positions. On the Mac a plain
   buffer; on the SPU a DMA ring (see spu/source). data must stay readable 32 bytes past data_bytes. */
typedef struct cabac_src cabac_src;

int cabac_parse_slice_exact(const cabac_slice_params *p, const uint8_t *data, cabac_out *out, cabac_stats *st);
int cabac_parse_slice_fast(const cabac_slice_params *p, const uint8_t *data, cabac_out *out, cabac_stats *st);
/* the fast engine without bin statistics (timing), Mac only */
int cabac_parse_slice_fastn(const cabac_slice_params *p, const uint8_t *data, cabac_out *out, cabac_stats *st);

#endif

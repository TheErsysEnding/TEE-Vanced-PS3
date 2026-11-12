/* TEE VP9 building block - made by tools/make_baustein.sh from common/vp9_dec.h; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9 SPU Test: the VP9 decoder's shared types - profile 0 (8 bit 4:2:0).

   Two stages, as the SPEs will run them:
     parse (vp9_parse.c)  headers, then per tile the boolean decoder: partitions, modes, motion vectors (with
                          their candidate search, which the parse needs: the inter mode's context and whether a
                          vector has 1/8-pel precision depend on it), coefficients; per block one record into a
                          records stream; after the frame the backward probability adaptation. One source for the
                          Mac, the PPU and the SPU: ps3/spu/source/main.c includes it with VP9_SPU and its own
                          memory hooks (DMA ring for the bitstream, records out by DMA, vectors per superblock).
     recon (vp9_recon.c)  from the records: prediction, inverse transforms, loop filter.
   The rules follow libvpx (Google's VP9 decoder, BSD-3-Clause) closely - it is the reference this decoder is
   checked against picture by picture. */
#ifndef VP9_DEC_H
#define VP9_DEC_H

#include <stddef.h>
#include <stdint.h>

#include "vp9_tables.h"

#define VP9_MAX_TILE_COLS 64
#define VP9_BORDER        80        /* pixels around every plane (MVs may point up to 64 + 8 beyond the frame) */
#define VP9_FRAMES        12        /* frame buffers: 8 reference slots, the current one, the output, spare */
#ifndef VP9_MAX_MI_COLS
#define VP9_MAX_MI_COLS   512       /* 8x8 columns: frames up to 4096 pixels wide (the SPU build: 320, 2560 pixels) */
#endif

/* ---- the boolean decoder (libvpx's vpx_reader arithmetic with a 32-bit window, as libvpx runs on 32-bit hosts;
   the bits come out the same with any window size) ---------------------------------------------------------- */

typedef struct {
	uint32_t value;
	int32_t count;                      /* bits in value below its top 8 (libvpx's count; + LOTS_OF_BITS past the end) */
	uint32_t range;
	uint32_t pos, end, start;           /* byte positions in the frame: the next to load, the data's end, its start */
#ifndef VP9_SPU
	const uint8_t *src;                 /* the frame */
#endif
} vp9_bool;

#define VP9_LOTS_OF_BITS 0x40000000

/* ---- probabilities and counts ------------------------------------------------------------------------------ */

typedef struct {
	uint8_t y_mode_prob[BLOCK_SIZE_GROUPS][INTRA_MODES - 1];
	uint8_t uv_mode_prob[INTRA_MODES][INTRA_MODES - 1];
	uint8_t partition_prob[PARTITION_CONTEXTS][PARTITION_TYPES - 1];
	uint8_t coef_probs[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS][UNCONSTRAINED_NODES];
	uint8_t switchable_interp_prob[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS - 1];
	uint8_t inter_mode_probs[INTER_MODE_CONTEXTS][INTER_MODES - 1];
	uint8_t intra_inter_prob[INTRA_INTER_CONTEXTS];
	uint8_t comp_inter_prob[COMP_INTER_CONTEXTS];
	uint8_t single_ref_prob[REF_CONTEXTS][2];
	uint8_t comp_ref_prob[REF_CONTEXTS];
	vp9_tx_probs tx_probs;
	uint8_t skip_probs[SKIP_CONTEXTS];
	vp9_nmv_context nmvc;
	int initialized;
} vp9_fc;

typedef struct {
	uint32_t sign[2], classes[MV_CLASSES], class0[CLASS0_SIZE], bits[MV_OFFSET_BITS][2];
	uint32_t class0_fp[CLASS0_SIZE][MV_FP_SIZE], fp[MV_FP_SIZE], class0_hp[2], hp[2];
} vp9_mv_comp_counts;

/* 16-byte aligned: the SPUs exchange it by DMA when two of them share a frame's tiles */
typedef struct {
	uint32_t y_mode[BLOCK_SIZE_GROUPS][INTRA_MODES];
	uint32_t uv_mode[INTRA_MODES][INTRA_MODES];
	uint32_t partition[PARTITION_CONTEXTS][PARTITION_TYPES];
	uint32_t coef[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS][UNCONSTRAINED_NODES + 1];
	uint32_t eob_branch[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS];
	uint32_t switchable_interp[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS];
	uint32_t inter_mode[INTER_MODE_CONTEXTS][INTER_MODES];
	uint32_t intra_inter[INTRA_INTER_CONTEXTS][2];
	uint32_t comp_inter[COMP_INTER_CONTEXTS][2];
	uint32_t single_ref[REF_CONTEXTS][2][2];
	uint32_t comp_ref[REF_CONTEXTS][2];
	uint32_t tx32x32[TX_SIZE_CONTEXTS][TX_SIZES], tx16x16[TX_SIZE_CONTEXTS][TX_SIZES - 1], tx8x8[TX_SIZE_CONTEXTS][TX_SIZES - 2];
	uint32_t skip[SKIP_CONTEXTS][2];
	uint32_t mv_joints[MV_JOINTS];
	vp9_mv_comp_counts mv[2];
} __attribute__((aligned(16))) vp9_counts;

/* ---- mode info ------------------------------------------------------------------------------------------------ */

typedef union {
	uint32_t as_int;
	struct {
		int16_t row, col;
	} as_mv;
} vp9_int_mv;

/* the block being parsed */
typedef struct {
	uint8_t sb_type, mode, uv_mode, tx_size;
	uint8_t skip, segment_id, seg_id_predicted, interp_filter;
	int8_t ref_frame[2];
	uint8_t bmode[4];                   /* sub-8x8: the 4x4 blocks' modes (intra) or inter modes */
	vp9_int_mv mv[2];
	vp9_int_mv bmv[4][2];               /* sub-8x8: the 4x4 blocks' vectors */
} __attribute__((aligned(16))) vp9_mi;

/* what later blocks need of the block covering an 8x8 (contexts, motion vector candidates): 32 bytes */
typedef struct {
	vp9_int_mv mv[2];                   /* sub-8x8: those of its last 4x4 */
	union {
		vp9_int_mv bmv[2][2];           /* inter sub-8x8: the vectors of its 4x4 blocks 1 and 2 (block 3's are mv) */
		uint8_t bmode[4];               /* intra sub-8x8: its 4x4 blocks' modes */
	} u;
	int8_t ref_frame[2];
	uint8_t sb_type, mode;
	uint8_t skip, tx_size, interp_filter, seg_id_predicted;
} __attribute__((aligned(16))) vp9_nb;     /* two quadwords: copied with two loads and two stores */

/* the window of the superblock being parsed: its 8x8 rows and columns with the 3 above and the 3 to the left (motion
   vector candidates reach that far) */
#define VP9_WIN_STRIDE 16
#define VP9_WIN_ROWS   11
#define VP9_WIN(r, c)  (((r) + 3) * VP9_WIN_STRIDE + (c) + 3)    /* r, c: from -3 to 7, relative to the superblock */

typedef struct {
	int8_t ref_frame[2];
	vp9_int_mv mv[2];
} __attribute__((aligned(16))) vp9_mvref;  /* one quadword; a superblock's 64 are 1 KB */

/* ---- frame headers ------------------------------------------------------------------------------------------- */

enum { KEY_FRAME = 0, INTER_FRAME = 1 };
enum { SINGLE_REFERENCE = 0, COMPOUND_REFERENCE = 1, REFERENCE_MODE_SELECT = 2 };

typedef struct {
	int profile, show_existing_frame, existing_idx, frame_type, show_frame, error_resilient_mode, intra_only;
	int reset_frame_context, refresh_frame_flags, ref_frame_idx[3], ref_frame_sign_bias[MAX_REF_FRAMES];
	int width, height, mi_cols, mi_rows, sb64_cols, sb64_rows;
	int allow_high_precision_mv, interp_filter, refresh_frame_context, frame_parallel_decoding_mode, frame_context_idx;
	int lf_level, lf_sharpness, lf_delta_enabled, lf_ref_deltas[MAX_REF_FRAMES], lf_mode_deltas[2];
	int base_qindex, y_dc_delta_q, uv_dc_delta_q, uv_ac_delta_q, lossless;
	int seg_enabled, seg_update_map, seg_temporal_update, seg_update_data, seg_abs_delta;
	uint8_t seg_tree_probs[SEG_TREE_PROBS], seg_pred_probs[PREDICTION_PROBS];
	int seg_feature_enabled[MAX_SEGMENTS][SEG_LVL_MAX], seg_feature_data[MAX_SEGMENTS][SEG_LVL_MAX];
	int log2_tile_cols, log2_tile_rows;
	int header_bytes, compressed_bytes;     /* uncompressed header (bytes), compressed header */
	int tx_mode, reference_mode, comp_fixed_ref, comp_var_ref[2];
	int use_prev_frame_mvs;
	int16_t y_dequant[MAX_SEGMENTS][2], uv_dequant[MAX_SEGMENTS][2];
} vp9_header;

/* ---- records: the parse stage's output, the pixel stage's input --------------------------------------------- */

/* A stream of 16-bit units (so the records hash the same on either byte order), written front to back without ever
   going back. Per tile: a tile record, one record per block in decode order, an end record. A block record is a
   head, then for inter blocks 0..2 quadwords of vectors, then (VP9_RB_COEFS) its transform blocks, planes Y, U, V,
   each in raster order within the block, as eob, then if eob > 0 the count of non-zero coefficients and per
   coefficient its position and its dequantized value (int16); zero padding to 16 bytes. The loop filter's skip is
   not in the head: an inter block of 8x8 or more whose transform blocks all have eob 0 counts as skipped. */
#define VP9_REC_TILE_HI 0x5449              /* "TI" */
#define VP9_REC_TILE_LO 0x4C45              /* "LE" */
#define VP9_REC_END     0xFFFF

typedef struct {
	uint16_t magic[2];                  /* VP9_REC_TILE_HI, VP9_REC_TILE_LO */
	int16_t mi_col_start, mi_col_end, mi_row_start, mi_row_end;
	uint16_t pad[2];
} vp9_rec_tile;                         /* 16 bytes */

typedef struct {
	uint16_t end[2];                    /* VP9_REC_END, VP9_REC_END */
	uint16_t blocks[2], tx_blocks[2], coefs[2];     /* the tile's counts, high half first */
} vp9_rec_tile_end;                     /* 16 bytes */

#define VP9_RB_COEFS    1               /* transform data follow (the block is not skipped) */
#define VP9_RB_INTER    2
#define VP9_RB_COMPOUND 4

typedef struct {
	uint16_t mi_row, mi_col;
	uint16_t type_flags;                /* sb_type << 8 | VP9_RB_* */
	uint16_t tx_seg;                    /* tx_size << 8 | interp_filter << 4 | segment_id */
	uint16_t modes;                     /* mode << 8 | uv_mode (inter: the inter mode, of the last 4x4 for sub-8x8) */
	uint16_t refs;                      /* (uint8_t)ref_frame[0] << 8 | (uint8_t)ref_frame[1] */
	uint16_t x[2];                      /* intra: bmode[0] << 8 | bmode[1], bmode[2] << 8 | bmode[3];
	                                       inter: mv[0] (row, col; sub-8x8: of the last 4x4) */
} vp9_rec_block;                        /* 16 bytes; inter: a compound 8x8+ block adds a quadword (mv[1] row, col, 0..);
                                           a sub-8x8 block one per reference (its 4x4 blocks' vectors, row, col each) */

/* ---- the parse stage's state ------------------------------------------------------------------------------- */

enum { VP9_OK = 0, VP9_E_MARKER, VP9_E_PROFILE, VP9_E_SYNC, VP9_E_SRGB, VP9_E_REF_EMPTY, VP9_E_REF_SIZE, VP9_E_NO_COMPRESSED,
       VP9_E_TOO_WIDE, VP9_E_HEADERS_LONG, VP9_E_CONTEXT, VP9_E_CH_MARKER, VP9_E_CH_OVERREAD, VP9_E_TILE_SIZE,
       VP9_E_TILE_MARKER, VP9_E_CORRUPT, VP9_E_RECORDS_FULL, VP9_E_COEF_RANGE, VP9_E_TILE_OVERREAD, VP9_E_EXISTING_EMPTY,
       VP9_E_SEGMENTATION, VP9_E_NO_MEMORY, VP9_E_COUNT };

typedef struct {
	vp9_header h;
	vp9_fc fc;                          /* this frame's probabilities */
	vp9_fc frame_contexts[4];
	vp9_counts counts;
	int count_enabled;                  /* !error_resilient && !frame_parallel_decoding_mode */
	/* persistent across frames */
	int ref_valid[NUM_REF_FRAMES];
	int ref_width[NUM_REF_FRAMES], ref_height[NUM_REF_FRAMES];
	int last_width, last_height, last_show_frame, last_frame_type, last_intra_only;
	int lf_ref_deltas[MAX_REF_FRAMES], lf_mode_deltas[2];      /* persistent loop filter deltas */
	int seg_feature_enabled[MAX_SEGMENTS][SEG_LVL_MAX], seg_feature_data[MAX_SEGMENTS][SEG_LVL_MAX], seg_abs_delta;
	uint8_t seg_tree_probs[SEG_TREE_PROBS], seg_pred_probs[PREDICTION_PROBS];
	uint32_t tile_data;                 /* the first tile's position in the frame */
	/* the coefficient reader's: this frame's coefficient probabilities, a word per (band, context) with the three
	   nodes in its upper bytes (expand_coef_probs), and the token cache in scan order, all zero between blocks */
	uint32_t xprobs[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS * COEFF_CONTEXTS] __attribute__((aligned(16)));
	uint32_t token_cache[32 * 32] __attribute__((aligned(16)));
	/* above contexts (whole frame width) */
	uint8_t above_partition[VP9_MAX_MI_COLS + 16] __attribute__((aligned(16)));   /* per 8x8 column */
	uint8_t above_nz[3][VP9_MAX_MI_COLS * 2 + 64] __attribute__((aligned(16)));   /* per 4x4 column of each plane */
	/* mode info: the 3 rows above the superblock row (whole width), the superblock's window */
	vp9_nb above3[3][VP9_MAX_MI_COLS];
	vp9_nb win[VP9_WIN_ROWS * VP9_WIN_STRIDE];
	/* records */
	uint8_t *ow;                        /* where the next record byte goes */
#ifdef VP9_SPU
	vp9_mvref *sb_prev, *sb_cur;        /* the superblock's vectors of the previous / this frame (local store) */
#else
	uint8_t *oend;
	int overflow;
	const uint8_t *src;                 /* the frame */
	int mi_alloc;
	vp9_mvref *mvs, *prev_mvs;          /* this frame's and the previous decoded frame's (inter frames only) */
	uint8_t *seg_map, *last_seg_map;
#endif
	/* the last frame: statistics, error */
	long tile_slack_bits_max;
	uint32_t blocks, tx_blocks, coefs;
	uint32_t tk_modes, tk_tokens, tk_rest;  /* the SPU's timebase ticks in the blocks: mode info (with the vector
	                                           candidates), transform data, the rest (records, contexts, window) */
	int error, error_a, error_b;
} vp9_parser;

/* one frame (one of a superframe's frames): headers, all tiles into `records` (at most `cap` bytes, 16-byte aligned).
   Returns the bytes of records written, 0 for a frame without data (show_existing_frame), -1 on an error (err says
   what). (The Mac and the PPU; the SPU runs the same steps through its own entry.) */
long vp9_parse_frame(vp9_parser *p, const uint8_t *data, size_t size, uint8_t *records, size_t cap, char *err, int errlen);
void vp9_parser_init(vp9_parser *p);
void vp9_parser_free(vp9_parser *p);
void vp9_error_text(const vp9_parser *p, char *buf, int len);

/* superframes: the frames of one packet; returns their count (1 without an index) */
int  vp9_superframe(const uint8_t *data, size_t size, const uint8_t **frames, size_t *sizes, int max);

/* FNV-1a over the records' 16-bit units (the same on either byte order) */
static inline uint32_t vp9_records_hash(uint32_t h, const uint8_t *rec, size_t bytes)
{
	const uint16_t *u = (const uint16_t *)rec;
	size_t i;

	for (i = 0; i < bytes / 2; i++)
		h = (h ^ u[i]) * 0x01000193u;
	return h;
}
#define VP9_HASH_INIT 0x811C9DC5u

#endif

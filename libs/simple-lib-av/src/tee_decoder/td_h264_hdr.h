/* TEE Decoder building block - made by tools/make_baustein.sh from common/h264_hdr.h; do not edit here: change the
   project's file and run the script again. */
/* H.264 high-level syntax for the CABAC test: Annex B NAL units, emulation prevention, SPS, PPS and
   the slice header - only as far as an 8-bit 4:2:0 progressive High Profile stream (as x264 makes
   them) needs. Plain C99, used on the Mac and on the PS3's PPU. */
#ifndef H264_HDR_H
#define H264_HDR_H

#include <stddef.h>
#include <stdint.h>

/* MSB-first bit reader over an RBSP (emulation prevention bytes already removed) */
typedef struct {
	const uint8_t *buf;
	size_t size;     /* bytes */
	size_t pos;      /* bits */
	int error;       /* read past the end */
} h264_bits;

void     h264_bits_init(h264_bits *b, const uint8_t *buf, size_t size);
uint32_t h264_u(h264_bits *b, int n);    /* n <= 32 */
uint32_t h264_ue(h264_bits *b);
int32_t  h264_se(h264_bits *b);

/* Annex B: the next NAL unit at or after *pos (start code 00 00 01 / 00 00 00 01); returns its first
   byte (the NAL header) and its length without trailing zero bytes, or NULL when there is none */
const uint8_t *h264_next_nal(const uint8_t *data, size_t size, size_t *pos, size_t *nal_len);

/* Annex B: the next access unit (one picture's NAL units, start codes included) at or after *pos: a new
   access unit starts at an access unit delimiter, SEI, SPS or PPS after a slice, or at a slice with
   first_mb_in_slice 0. Returns 1 with [*au, *au + *len), 0 at the end. (TEE Decoder 1.3: the input of a
   decoder that is fed picture by picture.) */
int h264_next_au(const uint8_t *data, size_t size, size_t *pos, const uint8_t **au, size_t *len);

/* NAL payload -> RBSP: drops the 0x03 of every 00 00 03; returns the RBSP length. out needs nal_len
   bytes. The NAL header byte is copied too (rbsp[0]). */
size_t h264_unescape(const uint8_t *nal, size_t nal_len, uint8_t *out);

/* position of the last 1 bit in an RBSP (the rbsp_stop_one_bit), counted in bits from the start;
   -1 if the RBSP is all zero */
long h264_stop_bit(const uint8_t *rbsp, size_t size);

typedef struct {
	int valid;
	int profile_idc, level_idc, sps_id;
	int chroma_format_idc, bit_depth_luma, bit_depth_chroma;
	int log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
	int num_ref_frames, frame_mbs_only, mb_adaptive_frame_field, direct_8x8_inference;
	int offset_for_non_ref_pic, offset_for_top_to_bottom_field, num_ref_frames_in_poc_cycle;
	int offset_for_ref_frame[256];
	int mb_width, mb_height;           /* in macroblocks */
	int crop_left, crop_right, crop_top, crop_bottom;
	int scaling_matrix_present;
	/* from the VUI (TEE Decoder); -1 when the stream does not say */
	int max_num_reorder_frames, max_dec_frame_buffering;
	int log2_max_mv_length_h, log2_max_mv_length_v;
	uint32_t num_units_in_tick, time_scale;    /* 0 when there is no timing info */
} h264_sps;

typedef struct {
	int valid;
	int pps_id, sps_id;
	int entropy_coding_mode;           /* 1 = CABAC */
	int bottom_field_pic_order_in_frame_present;
	int num_slice_groups;
	int num_ref_idx_l0_default, num_ref_idx_l1_default;   /* active counts, not minus 1 */
	int weighted_pred, weighted_bipred_idc;
	int pic_init_qp, pic_init_qs, chroma_qp_index_offset, second_chroma_qp_index_offset;
	int deblocking_filter_control_present, constrained_intra_pred, redundant_pic_cnt_present;
	int transform_8x8_mode, scaling_matrix_present;
} h264_pps;

enum { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2, SLICE_SP = 3, SLICE_SI = 4 };

#define H264_MAX_REFS  32
#define H264_MAX_MODS  34
#define H264_MAX_MMCO  66

typedef struct {
	int nal_unit_type, nal_ref_idc;
	int first_mb, slice_type, pps_id, frame_num, idr_pic_id;
	int poc_lsb, delta_poc_bottom, delta_poc[2];
	int direct_spatial_mv_pred;
	int num_ref_idx_l0, num_ref_idx_l1;   /* active counts */
	int cabac_init_idc, slice_qp, disable_deblocking;
	int alpha_offset, beta_offset;        /* slice_alpha_c0_offset_div2 << 1, slice_beta_offset_div2 << 1 */
	/* ref_pic_list_modification(): per list, n commands (idc, value), ended by idc 3 (not stored) */
	int mod_count[2];
	int mod_idc[2][H264_MAX_MODS], mod_val[2][H264_MAX_MODS];
	/* pred_weight_table() */
	int has_weights;
	int luma_log2_denom, chroma_log2_denom;
	int luma_weight[2][H264_MAX_REFS], luma_offset[2][H264_MAX_REFS];
	int chroma_weight[2][H264_MAX_REFS][2], chroma_offset[2][H264_MAX_REFS][2];
	int luma_weight_flag[2][H264_MAX_REFS], chroma_weight_flag[2][H264_MAX_REFS];
	/* dec_ref_pic_marking() */
	int no_output_of_prior_pics, long_term_reference;
	int adaptive_marking, mmco_count;
	int mmco_op[H264_MAX_MMCO], mmco_a[H264_MAX_MMCO], mmco_b[H264_MAX_MMCO];
	size_t data_bit;                      /* where slice_data() begins in the RBSP, in bits */
	size_t data_byte;                     /* the same after cabac_alignment_one_bit (CABAC) */
} h264_slice_hdr;

int h264_parse_sps(const uint8_t *rbsp, size_t size, h264_sps *sps);   /* 0 = ok */
int h264_parse_pps(const uint8_t *rbsp, size_t size, const h264_sps *sps_table, h264_pps *pps);
/* sps_table/pps_table are indexed by id (32 / 256 entries) */
int h264_parse_slice_hdr(const uint8_t *rbsp, size_t size, const h264_sps *sps_table,
                         const h264_pps *pps_table, h264_slice_hdr *sh);

#endif

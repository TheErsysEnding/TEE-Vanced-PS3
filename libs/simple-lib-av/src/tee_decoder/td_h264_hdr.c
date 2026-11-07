/* TEE Decoder building block - made by tools/make_baustein.sh from common/h264_hdr.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* H.264 high-level syntax for the CABAC test - see h264_hdr.h */
#include "td_h264_hdr.h"

#include <string.h>

void h264_bits_init(h264_bits *b, const uint8_t *buf, size_t size)
{
	b->buf = buf;
	b->size = size;
	b->pos = 0;
	b->error = 0;
}

static uint32_t bit1(h264_bits *b)
{
	uint32_t v;

	if (b->pos >= b->size * 8) {
		b->error = 1;
		return 0;
	}
	v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
	b->pos++;
	return v;
}

uint32_t h264_u(h264_bits *b, int n)
{
	uint32_t v = 0;

	while (n-- > 0)
		v = (v << 1) | bit1(b);
	return v;
}

uint32_t h264_ue(h264_bits *b)
{
	int zeros = 0;

	while (bit1(b) == 0) {
		if (b->error || ++zeros > 31) {
			b->error = 1;
			return 0;
		}
	}
	return ((1u << zeros) - 1) + h264_u(b, zeros);
}

int32_t h264_se(h264_bits *b)
{
	uint32_t k = h264_ue(b);

	return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

const uint8_t *h264_next_nal(const uint8_t *data, size_t size, size_t *pos, size_t *nal_len)
{
	size_t i = *pos, start, end;

	/* find 00 00 01 */
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
		i++;
	if (i + 3 > size)
		return NULL;
	start = i + 3;
	i = start;
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || data[i + 2] == 0)))
		i++;
	end = (i + 3 <= size) ? i : size;
	while (end > start && data[end - 1] == 0)   /* trailing_zero_8bits / the 00 of 00 00 00 01 */
		end--;
	*pos = (i + 3 <= size) ? i : size;
	*nal_len = end - start;
	return data + start;
}

/* the next start code (00 00 01) at or after i: its position (the first 00, an extra leading 00 included), or
   size */
static size_t next_start(const uint8_t *data, size_t size, size_t i, size_t *payload)
{
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
		i++;
	if (i + 3 > size) {
		*payload = size;
		return size;
	}
	*payload = i + 3;
	return (i > 0 && data[i - 1] == 0) ? i - 1 : i;     /* 00 00 00 01: the four-byte start code */
}

/* a NAL unit that begins an access unit once the current one has its slice */
static int starts_au(const uint8_t *data, size_t size, size_t payload)
{
	int t;

	if (payload >= size)
		return 0;
	t = data[payload] & 31;
	return t == 6 || t == 7 || t == 8 || t == 9 || ((t == 1 || t == 5) && payload + 1 < size && (data[payload + 1] & 0x80));
}

int h264_next_au(const uint8_t *data, size_t size, size_t *pos, const uint8_t **au, size_t *len)
{
	size_t payload, next, s, start = next_start(data, size, *pos, &payload);
	int has_slice = 0;

	if (start >= size)
		return 0;
	for (;;) {
		int t = payload < size ? data[payload] & 31 : 0;

		if (t == 1 || t == 5)
			has_slice = 1;
		s = next_start(data, size, payload, &next);
		if (s >= size || (has_slice && starts_au(data, size, next))) {
			*au = data + start;
			*len = (s >= size ? size : s) - start;
			*pos = s >= size ? size : s;
			return 1;
		}
		payload = next;
	}
}

size_t h264_unescape(const uint8_t *nal, size_t nal_len, uint8_t *out)
{
	size_t i, n = 0;
	int zeros = 0;

	for (i = 0; i < nal_len; i++) {
		uint8_t c = nal[i];

		if (zeros >= 2 && c == 3) {   /* emulation_prevention_three_byte */
			zeros = 0;
			continue;
		}
		out[n++] = c;
		zeros = (c == 0) ? zeros + 1 : 0;
	}
	return n;
}

long h264_stop_bit(const uint8_t *rbsp, size_t size)
{
	size_t i = size;

	while (i > 0 && rbsp[i - 1] == 0)
		i--;
	if (i == 0)
		return -1;
	{
		uint8_t c = rbsp[i - 1];
		int bit = 7;

		while (!((c >> (7 - bit)) & 1))   /* lowest set bit */
			bit--;
		return (long)((i - 1) * 8 + (size_t)bit);
	}
}

static void skip_scaling_list(h264_bits *b, int size)
{
	int last = 8, next = 8, j;

	for (j = 0; j < size; j++) {
		if (next != 0) {
			int delta = h264_se(b);

			next = (last + delta + 256) % 256;
		}
		last = (next == 0) ? last : next;
	}
}

/* hrd_parameters() (E.1.2), skipped */
static void skip_hrd(h264_bits *b)
{
	uint32_t cpb_cnt = h264_ue(b) + 1, i;

	h264_u(b, 8);                             /* bit_rate_scale, cpb_size_scale */
	for (i = 0; i < cpb_cnt && i < 32 && !b->error; i++) {
		h264_ue(b);
		h264_ue(b);
		h264_u(b, 1);
	}
	h264_u(b, 20);                            /* the four delay / offset lengths */
}

static int more_rbsp_data(const h264_bits *b)
{
	long stop = h264_stop_bit(b->buf, b->size);

	return stop >= 0 && (long)b->pos < stop;
}

int h264_parse_sps(const uint8_t *rbsp, size_t size, h264_sps *sps)
{
	h264_bits b;
	int i;

	memset(sps, 0, sizeof(*sps));
	h264_bits_init(&b, rbsp + 1, size - 1);   /* past the NAL header */
	sps->profile_idc = (int)h264_u(&b, 8);
	h264_u(&b, 8);                            /* constraint flags, reserved */
	sps->level_idc = (int)h264_u(&b, 8);
	sps->sps_id = (int)h264_ue(&b);
	sps->chroma_format_idc = 1;
	sps->bit_depth_luma = sps->bit_depth_chroma = 8;
	if (sps->profile_idc == 100 || sps->profile_idc == 110 || sps->profile_idc == 122 ||
	    sps->profile_idc == 244 || sps->profile_idc == 44 || sps->profile_idc == 83 ||
	    sps->profile_idc == 86 || sps->profile_idc == 118 || sps->profile_idc == 128 ||
	    sps->profile_idc == 138 || sps->profile_idc == 139 || sps->profile_idc == 134 ||
	    sps->profile_idc == 135) {
		sps->chroma_format_idc = (int)h264_ue(&b);
		if (sps->chroma_format_idc == 3)
			h264_u(&b, 1);                    /* separate_colour_plane_flag */
		sps->bit_depth_luma = 8 + (int)h264_ue(&b);
		sps->bit_depth_chroma = 8 + (int)h264_ue(&b);
		h264_u(&b, 1);                        /* qpprime_y_zero_transform_bypass_flag */
		sps->scaling_matrix_present = (int)h264_u(&b, 1);
		if (sps->scaling_matrix_present)
			for (i = 0; i < ((sps->chroma_format_idc != 3) ? 8 : 12); i++)
				if (h264_u(&b, 1))
					skip_scaling_list(&b, i < 6 ? 16 : 64);
	}
	sps->log2_max_frame_num = 4 + (int)h264_ue(&b);
	sps->poc_type = (int)h264_ue(&b);
	if (sps->poc_type == 0) {
		sps->log2_max_poc_lsb = 4 + (int)h264_ue(&b);
	} else if (sps->poc_type == 1) {
		int n;

		sps->delta_pic_order_always_zero = (int)h264_u(&b, 1);
		sps->offset_for_non_ref_pic = h264_se(&b);
		sps->offset_for_top_to_bottom_field = h264_se(&b);
		n = (int)h264_ue(&b);
		if (n > 255)
			return -1;
		sps->num_ref_frames_in_poc_cycle = n;
		for (i = 0; i < n; i++)
			sps->offset_for_ref_frame[i] = h264_se(&b);
	}
	sps->num_ref_frames = (int)h264_ue(&b);
	h264_u(&b, 1);                            /* gaps_in_frame_num_value_allowed_flag */
	sps->mb_width = 1 + (int)h264_ue(&b);
	sps->mb_height = 1 + (int)h264_ue(&b);
	sps->frame_mbs_only = (int)h264_u(&b, 1);
	if (!sps->frame_mbs_only) {
		sps->mb_adaptive_frame_field = (int)h264_u(&b, 1);
		sps->mb_height *= 2;
	}
	sps->direct_8x8_inference = (int)h264_u(&b, 1);
	if (h264_u(&b, 1)) {
		sps->crop_left = (int)h264_ue(&b);
		sps->crop_right = (int)h264_ue(&b);
		sps->crop_top = (int)h264_ue(&b);
		sps->crop_bottom = (int)h264_ue(&b);
	}
	/* the VUI (E.1.1): the TEE Decoder needs max_num_reorder_frames (output order) and the frame rate */
	sps->max_num_reorder_frames = -1;
	sps->max_dec_frame_buffering = -1;
	sps->log2_max_mv_length_h = sps->log2_max_mv_length_v = -1;
	if (h264_u(&b, 1)) {
		int nal_hrd, vcl_hrd;

		if (h264_u(&b, 1) && h264_u(&b, 8) == 255) {        /* aspect_ratio_idc: Extended_SAR */
			h264_u(&b, 16);
			h264_u(&b, 16);
		}
		if (h264_u(&b, 1))                                  /* overscan_info_present_flag */
			h264_u(&b, 1);
		if (h264_u(&b, 1)) {                                /* video_signal_type_present_flag */
			h264_u(&b, 4);                                  /* video_format, video_full_range_flag */
			if (h264_u(&b, 1))
				h264_u(&b, 24);                             /* colour primaries, transfer, matrix */
		}
		if (h264_u(&b, 1)) {                                /* chroma_loc_info_present_flag */
			h264_ue(&b);
			h264_ue(&b);
		}
		if (h264_u(&b, 1)) {                                /* timing_info_present_flag */
			sps->num_units_in_tick = h264_u(&b, 32);
			sps->time_scale = h264_u(&b, 32);
			h264_u(&b, 1);                                  /* fixed_frame_rate_flag */
		}
		nal_hrd = (int)h264_u(&b, 1);
		if (nal_hrd)
			skip_hrd(&b);
		vcl_hrd = (int)h264_u(&b, 1);
		if (vcl_hrd)
			skip_hrd(&b);
		if (nal_hrd || vcl_hrd)
			h264_u(&b, 1);                                  /* low_delay_hrd_flag */
		h264_u(&b, 1);                                      /* pic_struct_present_flag */
		if (h264_u(&b, 1)) {                                /* bitstream_restriction_flag */
			h264_u(&b, 1);                                  /* motion_vectors_over_pic_boundaries_flag */
			h264_ue(&b);                                    /* max_bytes_per_pic_denom */
			h264_ue(&b);                                    /* max_bits_per_mb_denom */
			sps->log2_max_mv_length_h = (int)h264_ue(&b);
			sps->log2_max_mv_length_v = (int)h264_ue(&b);
			sps->max_num_reorder_frames = (int)h264_ue(&b);
			sps->max_dec_frame_buffering = (int)h264_ue(&b);
		}
	}
	if (b.error)
		return -1;
	sps->valid = 1;
	return 0;
}

int h264_parse_pps(const uint8_t *rbsp, size_t size, const h264_sps *sps_table, h264_pps *pps)
{
	h264_bits b;
	const h264_sps *sps;

	memset(pps, 0, sizeof(*pps));
	h264_bits_init(&b, rbsp + 1, size - 1);
	pps->pps_id = (int)h264_ue(&b);
	pps->sps_id = (int)h264_ue(&b);
	if (pps->pps_id > 255 || pps->sps_id > 31 || !sps_table[pps->sps_id].valid)
		return -1;
	sps = &sps_table[pps->sps_id];
	pps->entropy_coding_mode = (int)h264_u(&b, 1);
	pps->bottom_field_pic_order_in_frame_present = (int)h264_u(&b, 1);
	pps->num_slice_groups = 1 + (int)h264_ue(&b);
	if (pps->num_slice_groups != 1)
		return -2;                            /* FMO: not in High Profile */
	pps->num_ref_idx_l0_default = 1 + (int)h264_ue(&b);
	pps->num_ref_idx_l1_default = 1 + (int)h264_ue(&b);
	pps->weighted_pred = (int)h264_u(&b, 1);
	pps->weighted_bipred_idc = (int)h264_u(&b, 2);
	pps->pic_init_qp = 26 + h264_se(&b);
	pps->pic_init_qs = 26 + h264_se(&b);
	pps->chroma_qp_index_offset = h264_se(&b);
	pps->deblocking_filter_control_present = (int)h264_u(&b, 1);
	pps->constrained_intra_pred = (int)h264_u(&b, 1);
	pps->redundant_pic_cnt_present = (int)h264_u(&b, 1);
	pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
	if (more_rbsp_data(&b)) {
		int i;

		pps->transform_8x8_mode = (int)h264_u(&b, 1);
		pps->scaling_matrix_present = (int)h264_u(&b, 1);
		if (pps->scaling_matrix_present)
			for (i = 0; i < 6 + ((sps->chroma_format_idc != 3) ? 2 : 6) * pps->transform_8x8_mode; i++)
				if (h264_u(&b, 1))
					skip_scaling_list(&b, i < 6 ? 16 : 64);
		pps->second_chroma_qp_index_offset = h264_se(&b);
	}
	if (b.error)
		return -1;
	pps->valid = 1;
	return 0;
}

int h264_parse_slice_hdr(const uint8_t *rbsp, size_t size, const h264_sps *sps_table,
                         const h264_pps *pps_table, h264_slice_hdr *sh)
{
	h264_bits b;
	const h264_sps *sps;
	const h264_pps *pps;
	int idr, i;

	memset(sh, 0, sizeof(*sh));
	sh->nal_ref_idc = (rbsp[0] >> 5) & 3;
	sh->nal_unit_type = rbsp[0] & 31;
	idr = sh->nal_unit_type == 5;
	h264_bits_init(&b, rbsp + 1, size - 1);
	sh->first_mb = (int)h264_ue(&b);
	sh->slice_type = (int)(h264_ue(&b) % 5);
	sh->pps_id = (int)h264_ue(&b);
	if (sh->pps_id > 255 || !pps_table[sh->pps_id].valid)
		return -1;
	pps = &pps_table[sh->pps_id];
	sps = &sps_table[pps->sps_id];
	if (!sps->valid || !sps->frame_mbs_only)
		return -2;                            /* interlaced: not handled */
	sh->frame_num = (int)h264_u(&b, sps->log2_max_frame_num);
	if (idr)
		sh->idr_pic_id = (int)h264_ue(&b);
	if (sps->poc_type == 0) {
		sh->poc_lsb = (int)h264_u(&b, sps->log2_max_poc_lsb);
		if (pps->bottom_field_pic_order_in_frame_present)
			sh->delta_poc_bottom = h264_se(&b);
	} else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
		sh->delta_poc[0] = h264_se(&b);
		if (pps->bottom_field_pic_order_in_frame_present)
			sh->delta_poc[1] = h264_se(&b);
	}
	if (pps->redundant_pic_cnt_present)
		h264_ue(&b);
	if (sh->slice_type == SLICE_B)
		sh->direct_spatial_mv_pred = (int)h264_u(&b, 1);
	sh->num_ref_idx_l0 = pps->num_ref_idx_l0_default;
	sh->num_ref_idx_l1 = pps->num_ref_idx_l1_default;
	if (sh->slice_type == SLICE_P || sh->slice_type == SLICE_SP || sh->slice_type == SLICE_B) {
		if (h264_u(&b, 1)) {
			sh->num_ref_idx_l0 = 1 + (int)h264_ue(&b);
			if (sh->slice_type == SLICE_B)
				sh->num_ref_idx_l1 = 1 + (int)h264_ue(&b);
		}
	}
	if (sh->slice_type != SLICE_B)
		sh->num_ref_idx_l1 = 0;
	if (sh->slice_type == SLICE_I || sh->slice_type == SLICE_SI)
		sh->num_ref_idx_l0 = 0;
	/* ref_pic_list_modification() */
	for (i = 0; i < (sh->slice_type == SLICE_B ? 2 : (sh->num_ref_idx_l0 ? 1 : 0)); i++) {
		if (h264_u(&b, 1)) {
			uint32_t idc;

			do {
				idc = h264_ue(&b);
				if (idc == 0 || idc == 1 || idc == 2) {
					uint32_t v = h264_ue(&b);

					if (sh->mod_count[i] >= H264_MAX_MODS)
						return -5;
					sh->mod_idc[i][sh->mod_count[i]] = (int)idc;
					sh->mod_val[i][sh->mod_count[i]++] = (int)v;
				} else if (idc != 3) {
					return -5;
				}
			} while (idc != 3 && !b.error);
		}
	}
	/* pred_weight_table() */
	if ((pps->weighted_pred && (sh->slice_type == SLICE_P || sh->slice_type == SLICE_SP)) ||
	    (pps->weighted_bipred_idc == 1 && sh->slice_type == SLICE_B)) {
		int list, chroma = sps->chroma_format_idc != 0;

		sh->has_weights = 1;
		sh->luma_log2_denom = (int)h264_ue(&b);
		if (chroma)
			sh->chroma_log2_denom = (int)h264_ue(&b);
		for (list = 0; list < (sh->slice_type == SLICE_B ? 2 : 1); list++) {
			int n = list ? sh->num_ref_idx_l1 : sh->num_ref_idx_l0;

			for (i = 0; i < n && i < H264_MAX_REFS; i++) {
				sh->luma_weight[list][i] = 1 << sh->luma_log2_denom;
				sh->luma_offset[list][i] = 0;
				sh->chroma_weight[list][i][0] = sh->chroma_weight[list][i][1] = 1 << sh->chroma_log2_denom;
				sh->chroma_offset[list][i][0] = sh->chroma_offset[list][i][1] = 0;
				if ((sh->luma_weight_flag[list][i] = (int)h264_u(&b, 1)) != 0) {
					sh->luma_weight[list][i] = h264_se(&b);
					sh->luma_offset[list][i] = h264_se(&b);
				}
				if (chroma && (sh->chroma_weight_flag[list][i] = (int)h264_u(&b, 1)) != 0) {
					int j;

					for (j = 0; j < 2; j++) {
						sh->chroma_weight[list][i][j] = h264_se(&b);
						sh->chroma_offset[list][i][j] = h264_se(&b);
					}
				}
			}
		}
	}
	/* dec_ref_pic_marking() */
	if (sh->nal_ref_idc) {
		if (idr) {
			sh->no_output_of_prior_pics = (int)h264_u(&b, 1);
			sh->long_term_reference = (int)h264_u(&b, 1);
		} else if ((sh->adaptive_marking = (int)h264_u(&b, 1)) != 0) {
			uint32_t op;

			do {
				int a = 0, c = 0;

				op = h264_ue(&b);
				if (op == 1 || op == 3)
					a = (int)h264_ue(&b);             /* difference_of_pic_nums_minus1 */
				if (op == 2)
					a = (int)h264_ue(&b);             /* long_term_pic_num */
				if (op == 3 || op == 6)
					c = (int)h264_ue(&b);             /* long_term_frame_idx */
				if (op == 4)
					a = (int)h264_ue(&b);             /* max_long_term_frame_idx_plus1 */
				if (op > 6 || sh->mmco_count >= H264_MAX_MMCO)
					return -6;
				if (op != 0) {
					sh->mmco_op[sh->mmco_count] = (int)op;
					sh->mmco_a[sh->mmco_count] = a;
					sh->mmco_b[sh->mmco_count++] = c;
				}
			} while (op != 0 && !b.error);
		}
	}
	if (pps->entropy_coding_mode && sh->slice_type != SLICE_I && sh->slice_type != SLICE_SI)
		sh->cabac_init_idc = (int)h264_ue(&b);
	sh->slice_qp = pps->pic_init_qp + h264_se(&b);
	if (sh->slice_type == SLICE_SP || sh->slice_type == SLICE_SI) {
		if (sh->slice_type == SLICE_SP)
			h264_u(&b, 1);
		h264_se(&b);
	}
	if (pps->deblocking_filter_control_present) {
		sh->disable_deblocking = (int)h264_ue(&b);
		if (sh->disable_deblocking != 1) {
			sh->alpha_offset = h264_se(&b) * 2;
			sh->beta_offset = h264_se(&b) * 2;
		}
	}
	if (b.error || sh->cabac_init_idc > 2 || sh->slice_qp < 0 || sh->slice_qp > 51)
		return -3;
	/* positions in the whole RBSP (with its NAL header byte) */
	sh->data_bit = b.pos + 8;
	if (pps->entropy_coding_mode) {
		while (b.pos & 7) {
			if (h264_u(&b, 1) != 1)
				return -4;                    /* cabac_alignment_one_bit must be 1 */
		}
		sh->data_byte = (b.pos >> 3) + 1;
	}
	return 0;
}

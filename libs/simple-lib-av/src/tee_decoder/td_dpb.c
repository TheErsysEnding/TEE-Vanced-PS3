/* TEE Decoder building block - made by tools/make_baustein.sh from common/dpb.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* Decoded picture buffer - see dpb.h */
#include "td_dpb.h"

#include <string.h>

size_t dpb_slot_bytes(int width, int height)
{
	size_t sy = (size_t)width + 2 * RECON_BORDER, hy = (size_t)height + 2 * RECON_BORDER;
	size_t luma = sy * hy, chroma = (sy / 2) * (hy / 2), col = (size_t)(width / 16) * (height / 16) * 4 * sizeof(recon_col);

	return (luma + 2 * chroma + col + 512 + 127) & ~(size_t)127;
}

int dpb_init(dpb *d, int width, int height, int max_refs, int nslots, void *(*alloc)(size_t))
{
	int i, sy = width + 2 * RECON_BORDER, sc = sy / 2, hy = height + 2 * RECON_BORDER, hc = hy / 2;
	size_t luma = (size_t)sy * hy, chroma = (size_t)sc * hc;

	memset(d, 0, sizeof(*d));
	d->width = width;
	d->height = height;
	d->max_refs = max_refs < 1 ? 1 : max_refs;
	d->max_long_term_idx = -1;
	if (nslots <= 0)
		nslots = d->max_refs + 4;
	d->nslots = nslots < DPB_SLOTS ? nslots : DPB_SLOTS;
	for (i = 0; i < DPB_SLOTS; i++) {
		dpb_slot *s = &d->s[i];
		uint8_t *m;

		if (i >= d->nslots) {
			s->busy = 1 << 30;          /* never handed out */
			continue;
		}
		m = alloc(dpb_slot_bytes(width, height));

		if (!m)
			return -1;
		memset(m, 0, luma + 2 * chroma);
		s->mem = m;
		s->pic.stride_y = sy;
		s->pic.stride_c = sc;
		s->pic.y = m + (size_t)RECON_BORDER * sy + RECON_BORDER;
		s->pic.u = m + luma + (size_t)(RECON_BORDER / 2) * sc + RECON_BORDER / 2;
		s->pic.v = m + luma + chroma + (size_t)(RECON_BORDER / 2) * sc + RECON_BORDER / 2;
		s->pic.col = (recon_col *)(void *)(((uintptr_t)(m + luma + 2 * chroma) + 127) & ~(uintptr_t)127);
		s->pic.id = 0;
	}
	return 0;
}

void dpb_reset(dpb *d)
{
	int i;

	for (i = 0; i < DPB_SLOTS; i++) {
		dpb_slot *s = &d->s[i];

		s->ref = 0;
		s->busy = i < d->nslots ? 0 : 1 << 30;
		s->frame_num = s->frame_num_wrap = s->long_term_frame_idx = s->pic_num = s->long_term_pic_num = 0;
		s->pic.poc = s->pic.long_term = s->pic.id = 0;
	}
	d->max_long_term_idx = -1;
	d->prev_poc_msb = d->prev_poc_lsb = d->prev_frame_num = d->prev_frame_num_offset = d->prev_ref_had_mmco5 = 0;
	d->cur = 0;
	d->serial = 0;
}

int dpb_free_slots(const dpb *d, int ignore_refs)
{
	int i, n = 0;

	for (i = 0; i < d->nslots; i++)
		if (!d->s[i].busy && (ignore_refs || !d->s[i].ref))
			n++;
	return n;
}

static void sort_slots(dpb *d, int *v, int n, int (*key)(const dpb *, int), int descending)
{
	int i, j;

	for (i = 1; i < n; i++)
		for (j = i; j > 0; j--) {
			int ka = key(d, v[j - 1]), kb = key(d, v[j]);

			if (descending ? ka < kb : ka > kb) {
				int t = v[j - 1];

				v[j - 1] = v[j];
				v[j] = t;
			} else {
				break;
			}
		}
}

static int key_picnum(const dpb *d, int i) { return d->s[i].pic_num; }
static int key_ltpicnum(const dpb *d, int i) { return d->s[i].long_term_pic_num; }
static int key_poc(const dpb *d, int i) { return d->s[i].pic.poc; }

int dpb_begin(dpb *d, const h264_sps *sps, const h264_pps *pps, const h264_slice_hdr *sh, recon_params *rp)
{
	int idr = sh->nal_unit_type == 5, i, list, poc, max_frame_num = 1 << sps->log2_max_frame_num, cur = -1;
	int st[DPB_SLOTS], lt[DPB_SLOTS], nst = 0, nlt = 0;

	(void)pps;
	if (idr) {
		for (i = 0; i < DPB_SLOTS; i++)
			d->s[i].ref = 0;
		d->max_long_term_idx = -1;
	}
	/* picture order count */
	if (sps->poc_type == 0) {
		int max_lsb = 1 << sps->log2_max_poc_lsb, msb;

		if (idr) {
			d->prev_poc_msb = 0;
			d->prev_poc_lsb = 0;
		}
		if (sh->poc_lsb < d->prev_poc_lsb && d->prev_poc_lsb - sh->poc_lsb >= max_lsb / 2)
			msb = d->prev_poc_msb + max_lsb;
		else if (sh->poc_lsb > d->prev_poc_lsb && sh->poc_lsb - d->prev_poc_lsb > max_lsb / 2)
			msb = d->prev_poc_msb - max_lsb;
		else
			msb = d->prev_poc_msb;
		poc = msb + sh->poc_lsb;
		if (sh->delta_poc_bottom < 0)
			poc += sh->delta_poc_bottom;
		if (sh->nal_ref_idc) {
			d->prev_poc_msb = msb;
			d->prev_poc_lsb = sh->poc_lsb;
		}
	} else if (sps->poc_type == 2) {
		int off = idr ? 0 : d->prev_frame_num > sh->frame_num ? d->prev_frame_num_offset + max_frame_num : d->prev_frame_num_offset;

		poc = idr ? 0 : sh->nal_ref_idc ? 2 * (off + sh->frame_num) : 2 * (off + sh->frame_num) - 1;
		d->prev_frame_num_offset = off;
	} else {
		return -1;                      /* type 1: not needed for x264 streams */
	}
	d->prev_frame_num = sh->frame_num;
	/* a free slot */
	for (i = 0; i < DPB_SLOTS; i++)
		if (!d->s[i].ref && !d->s[i].busy) {
			cur = i;
			break;
		}
	if (cur < 0)
		return -1;
	d->cur = cur;
	d->s[cur].busy = 1;
	d->s[cur].ref = 0;
	d->s[cur].pic.id = (d->serial++ & 0x3FFF) + 1;
	d->s[cur].pic.poc = poc;
	d->s[cur].pic.long_term = 0;
	d->s[cur].frame_num = sh->frame_num;

	/* picture numbers (8.2.4.1) */
	for (i = 0; i < DPB_SLOTS; i++) {
		dpb_slot *s = &d->s[i];

		if (i == cur)
			continue;
		if (s->ref == 1) {
			s->frame_num_wrap = s->frame_num > sh->frame_num ? s->frame_num - max_frame_num : s->frame_num;
			s->pic_num = s->frame_num_wrap;
			st[nst++] = i;
		} else if (s->ref == 2) {
			s->long_term_pic_num = s->long_term_frame_idx;
			lt[nlt++] = i;
		}
	}
	memset(rp->ref, 0, sizeof(rp->ref));
	rp->num_ref[0] = sh->num_ref_idx_l0;
	rp->num_ref[1] = sh->num_ref_idx_l1;
	rp->cur_poc = poc;
	for (list = 0; list < (sh->slice_type == SLICE_B ? 2 : sh->slice_type == SLICE_P ? 1 : 0); list++) {
		int init[2 * DPB_SLOTS], n = 0, k, pred, ri;

		if (sh->slice_type == SLICE_P) {
			int a[DPB_SLOTS], b[DPB_SLOTS];

			memcpy(a, st, sizeof(int) * (size_t)nst);
			sort_slots(d, a, nst, key_picnum, 1);
			memcpy(b, lt, sizeof(int) * (size_t)nlt);
			sort_slots(d, b, nlt, key_ltpicnum, 0);
			for (k = 0; k < nst; k++)
				init[n++] = a[k];
			for (k = 0; k < nlt; k++)
				init[n++] = b[k];
		} else {
			int before[DPB_SLOTS], after[DPB_SLOTS], nb = 0, na = 0, b2[DPB_SLOTS];

			for (k = 0; k < nst; k++) {
				if (d->s[st[k]].pic.poc < poc)
					before[nb++] = st[k];
				else
					after[na++] = st[k];
			}
			sort_slots(d, before, nb, key_poc, 1);
			sort_slots(d, after, na, key_poc, 0);
			if (list == 0) {
				for (k = 0; k < nb; k++) init[n++] = before[k];
				for (k = 0; k < na; k++) init[n++] = after[k];
			} else {
				for (k = 0; k < na; k++) init[n++] = after[k];
				for (k = 0; k < nb; k++) init[n++] = before[k];
			}
			memcpy(b2, lt, sizeof(int) * (size_t)nlt);
			sort_slots(d, b2, nlt, key_ltpicnum, 0);
			for (k = 0; k < nlt; k++)
				init[n++] = b2[k];
		}
		/* the list as slots, then modification (8.2.4.3) */
		{
			int lst[RECON_MAX_REFS + 1], len = rp->num_ref[list];

			for (k = 0; k <= RECON_MAX_REFS; k++)
				lst[k] = k < n ? init[k] : -1;
			if (list == 1 && n > 1) {
				/* an initial L1 equal to the initial L0 gets its first two entries swapped: rebuild L0 */
				int l0[2 * DPB_SLOTS], n0 = 0, before[DPB_SLOTS], after[DPB_SLOTS], nb = 0, na = 0, same = 1;

				for (k = 0; k < nst; k++) {
					if (d->s[st[k]].pic.poc < poc)
						before[nb++] = st[k];
					else
						after[na++] = st[k];
				}
				sort_slots(d, before, nb, key_poc, 1);
				sort_slots(d, after, na, key_poc, 0);
				for (k = 0; k < nb; k++) l0[n0++] = before[k];
				for (k = 0; k < na; k++) l0[n0++] = after[k];
				{
					int b3[DPB_SLOTS];

					memcpy(b3, lt, sizeof(int) * (size_t)nlt);
					sort_slots(d, b3, nlt, key_ltpicnum, 0);
					for (k = 0; k < nlt; k++)
						l0[n0++] = b3[k];
				}
				for (k = 0; k < n; k++)
					if (l0[k] != init[k])
						same = 0;
				if (same) {
					lst[0] = init[1];
					lst[1] = init[0];
				}
			}
			pred = sh->frame_num;
			for (ri = 0; ri < sh->mod_count[list]; ri++) {
				int idc = sh->mod_idc[list][ri], v = sh->mod_val[list][ri], target = -1, cidx, nidx;

				if (idc == 0 || idc == 1) {
					int nowrap = idc == 0 ? pred - (v + 1) : pred + (v + 1), picnum;

					if (nowrap < 0)
						nowrap += max_frame_num;
					else if (nowrap >= max_frame_num)
						nowrap -= max_frame_num;
					pred = nowrap;
					picnum = nowrap > sh->frame_num ? nowrap - max_frame_num : nowrap;
					for (k = 0; k < nst; k++)
						if (d->s[st[k]].pic_num == picnum)
							target = st[k];
				} else {
					for (k = 0; k < nlt; k++)
						if (d->s[lt[k]].long_term_pic_num == v)
							target = lt[k];
				}
				if (target < 0)
					return -1;
				for (cidx = len; cidx > ri; cidx--)
					lst[cidx] = lst[cidx - 1];
				lst[ri] = target;
				nidx = ri + 1;
				for (cidx = ri + 1; cidx <= len; cidx++)
					if (lst[cidx] != target)
						lst[nidx++] = lst[cidx];
			}
			for (k = 0; k < len && k < RECON_MAX_REFS; k++)
				rp->ref[list][k] = lst[k] >= 0 ? &d->s[lst[k]].pic : NULL;
		}
	}
	return cur;
}

static void unmark_short(dpb *d, int picnum, int max_frame_num, int cur_frame_num)
{
	int i;

	for (i = 0; i < DPB_SLOTS; i++) {
		dpb_slot *s = &d->s[i];
		int wrap;

		if (s->ref != 1 || i == d->cur)
			continue;
		wrap = s->frame_num > cur_frame_num ? s->frame_num - max_frame_num : s->frame_num;
		if (wrap == picnum)
			s->ref = 0;
	}
}

void dpb_end(dpb *d, const h264_sps *sps, const h264_slice_hdr *sh)
{
	dpb_slot *cur = &d->s[d->cur];
	int i, max_frame_num = 1 << sps->log2_max_frame_num;

	if (!sh->nal_ref_idc)
		return;
	if (sh->nal_unit_type == 5) {
		cur->ref = sh->long_term_reference ? 2 : 1;
		cur->long_term_frame_idx = 0;
		cur->pic.long_term = sh->long_term_reference;
		d->max_long_term_idx = sh->long_term_reference ? 0 : -1;
		return;
	}
	if (sh->adaptive_marking) {
		int k, cur_long = 0;

		for (k = 0; k < sh->mmco_count; k++) {
			int op = sh->mmco_op[k], a = sh->mmco_a[k], b = sh->mmco_b[k];

			if (op == 1) {
				unmark_short(d, sh->frame_num - (a + 1), max_frame_num, sh->frame_num);
			} else if (op == 2) {
				for (i = 0; i < DPB_SLOTS; i++)
					if (d->s[i].ref == 2 && d->s[i].long_term_frame_idx == a)
						d->s[i].ref = 0;
			} else if (op == 3) {
				int picnum = sh->frame_num - (a + 1);

				for (i = 0; i < DPB_SLOTS; i++)
					if (d->s[i].ref == 2 && d->s[i].long_term_frame_idx == b)
						d->s[i].ref = 0;
				for (i = 0; i < DPB_SLOTS; i++) {
					dpb_slot *s = &d->s[i];
					int wrap = s->frame_num > sh->frame_num ? s->frame_num - max_frame_num : s->frame_num;

					if (s->ref == 1 && i != d->cur && wrap == picnum) {
						s->ref = 2;
						s->long_term_frame_idx = b;
						s->pic.long_term = 1;
					}
				}
			} else if (op == 4) {
				d->max_long_term_idx = a - 1;
				for (i = 0; i < DPB_SLOTS; i++)
					if (d->s[i].ref == 2 && d->s[i].long_term_frame_idx > d->max_long_term_idx)
						d->s[i].ref = 0;
			} else if (op == 5) {
				for (i = 0; i < DPB_SLOTS; i++)
					if (i != d->cur)
						d->s[i].ref = 0;
				d->max_long_term_idx = -1;
				cur->pic.poc = 0;
				d->prev_poc_msb = 0;
				d->prev_poc_lsb = 0;
				d->prev_frame_num = 0;
				d->prev_frame_num_offset = 0;
			} else if (op == 6) {
				for (i = 0; i < DPB_SLOTS; i++)
					if (d->s[i].ref == 2 && d->s[i].long_term_frame_idx == b && i != d->cur)
						d->s[i].ref = 0;
				cur->ref = 2;
				cur->long_term_frame_idx = b;
				cur->pic.long_term = 1;
				cur_long = 1;
			}
		}
		if (!cur_long)
			cur->ref = 1;
	} else {
		int nref = 0, oldest = -1;

		for (i = 0; i < DPB_SLOTS; i++)
			if (d->s[i].ref && i != d->cur)
				nref++;
		if (nref >= d->max_refs) {
			for (i = 0; i < DPB_SLOTS; i++) {
				dpb_slot *s = &d->s[i];
				int wrap;

				if (s->ref != 1 || i == d->cur)
					continue;
				wrap = s->frame_num > sh->frame_num ? s->frame_num - max_frame_num : s->frame_num;
				if (oldest < 0 || wrap < d->s[oldest].frame_num_wrap)
					oldest = i;
				s->frame_num_wrap = wrap;
			}
			if (oldest >= 0)
				d->s[oldest].ref = 0;
		}
		cur->ref = 1;
	}
	if (cur->ref == 1)
		cur->pic.long_term = 0;
}

void dpb_hold(dpb *d, int slot)
{
	d->s[slot].busy++;
}

void dpb_release(dpb *d, int slot)
{
	if (d->s[slot].busy > 0)
		d->s[slot].busy--;
}

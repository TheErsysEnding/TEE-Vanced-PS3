/* TEE VP9 building block - made by tools/make_baustein.sh from common/vp9_rsxmc.c; do not edit here: change the
   project's file and run the script again. */
/* RSX motion compensation: the rectangle builder and the RSX's passes in C (see vp9_rsxmc.h). Plain C: the PPU runs
   the builder on the console, the Mac simulation both. The windows follow the pixel SPU's plan_block exactly
   (ps3/spu_pixel/source/main.c). */
#include "vp9_rsxmc.h"

#include <stdlib.h>
#include <string.h>

#include "vp9_dec.h"
#include "vp9_tables.h"

static const uint8_t n8w[BLOCK_SIZES] = { 1, 1, 1, 1, 1, 2, 2, 2, 4, 4, 4, 8, 8 };
static const uint8_t n8h[BLOCK_SIZES] = { 1, 1, 1, 1, 2, 1, 2, 4, 2, 4, 8, 4, 8 };
static const uint8_t mtx[BLOCK_SIZES] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3 };
static const uint8_t uvb[BLOCK_SIZES] = { BLOCK_INVALID, BLOCK_INVALID, BLOCK_INVALID, BLOCK_4X4, BLOCK_4X8, BLOCK_8X4,
                                          BLOCK_8X8, BLOCK_8X16, BLOCK_16X8, BLOCK_16X16, BLOCK_16X32, BLOCK_32X16,
                                          BLOCK_32X32 };

static const int16_t (*const sets[4])[SUBPEL_TAPS] = { vp9_sub_pel_filters_8, vp9_sub_pel_filters_8lp,
                                                       vp9_sub_pel_filters_8s, vp9_bilinear_filters };

const int16_t *rsxmc_kernel(int s, int p)
{
	return sets[s & 3][p & 15];
}

void rsxmc_reset(rsxmc_lists *L)
{
	int i;

	for (i = 0; i < RSXMC_NLIST; i++)
		L->n[i] = 0;
	for (i = 0; i < 3; i++)
		L->need_y[i] = L->need_c[i] = (int32_t)0x80000000;
	L->blocks = L->inter = L->compound = L->rects = L->small = L->compound_rsx = 0;
	L->cls[0] = L->cls[1] = L->cls[2] = 0;
	L->failed = 0;
}

void rsxmc_free(rsxmc_lists *L)
{
	int i;

	for (i = 0; i < RSXMC_NLIST; i++) {
		free(L->r[i]);
		L->r[i] = NULL;
		L->cap[i] = L->n[i] = 0;
	}
}

static void put(rsxmc_lists *L, int list, int x0, int y0, int x1, int y1, int dx, int dy, int set, int pos)
{
	rsxmc_rect *r;

	if (L->n[list] == L->cap[list]) {
		const uint32_t cap = L->cap[list] ? L->cap[list] * 2 : 256;
		rsxmc_rect *p = realloc(L->r[list], cap * sizeof(*p));

		if (!p) {
			L->failed = 1;
			return;
		}
		L->r[list] = p;
		L->cap[list] = cap;
	}
	r = &L->r[list][L->n[list]++];
	r->x0 = (int16_t)x0;
	r->y0 = (int16_t)y0;
	r->x1 = (int16_t)x1;
	r->y1 = (int16_t)y1;
	r->dx = (int16_t)dx;
	r->dy = (int16_t)dy;
	r->set = (uint8_t)set;
	r->pos = (uint8_t)pos;
	r->pad[0] = r->pad[1] = 0;
	L->rects++;
}

/* a prediction's last pass: the w x h pixels at (x, y) of plane `plane` (superblock row sb_row), read from (sx, sy)
   of the pass's texture on, to where they lie in the prediction surface - or, cls 1, in the store surface:
     frame-shaped: the plane's place in the surface (the store: the row's band of it)
     packed:       the block's superblock column's 96 rows of the row's surface, luma on top, U | V below
     strips:       the superblock's cell; a luma block 64 high is two rectangles, one in each of the cell's halves */
static void put_last(rsxmc_lists *L, const rsxmc_geom *g, int list, int sb_row, int plane, int cls, int x, int y, int w, int h,
                     int sx, int sy, int set, int pos)
{
	const int pn = plane ? 32 : 64, xl = x & (pn - 1), yl = y - sb_row * pn;

	if (g->strips) {
		const int n = sb_row * (g->aw / 64) + x / pn;
		const int cx = (n % RSXMC_STRIP_PER) * RSXMC_STRIP_W, cy = (n / RSXMC_STRIP_PER) * RSXMC_STRIP_H;

		if (plane) {
			const int tx = cx + 128 + (plane == 2 ? 32 : 0) + xl, ty = cy + yl;

			put(L, list, tx, ty, tx + w, ty + h, sx - tx, sy - ty, set, pos);
		} else {
			int ya;

			for (ya = yl; ya < yl + h; ya = (ya | 31) + 1) {
				const int yb = (ya | 31) + 1 < yl + h ? (ya | 31) + 1 : yl + h;
				const int tx = cx + (ya >> 5) * 64 + xl, ty = cy + (ya & 31);

				put(L, list, tx, ty, tx + w, ty + (yb - ya), sx - tx, sy + (ya - yl) - ty, set, pos);
			}
		}
	} else if (g->packed) {
		const int tx = xl + (plane == 2 ? 32 : 0), ty = (x / pn) * 96 + (plane ? 64 : 0) + yl;

		put(L, list, tx, ty, tx + w, ty + h, sx - tx, sy - ty, set, pos);
	} else {
		const int tx = x + (plane == 2 ? g->aw / 2 : 0);
		const int ty = y + (plane ? g->ah : 0) + (cls == 1 ? RSXMC_STORE_DY(g, plane, sb_row) : 0);

		put(L, list, tx, ty, tx + w, ty + h, sx - tx, sy - ty, set, pos);
	}
}

#ifdef __APPLE__
/* the Mac model's statistics: [class][0 copy, 1 one pass with a filter, 2 two passes] -> predictions, pixels drawn */
unsigned long long rsxmc_pass[3][3][2];
#endif

/* cls: 0 a block with one reference; 1 a compound block's first prediction (its last pass into the store surface);
   2 its second one (lists of their own: the last pass averages with the store's pixel) */
static void window(rsxmc_lists *L, const rsxmc_geom *g, int sb_row, int plane, int k, int set, int x, int y, int w, int h,
                   int vr, int vc, int cls)
{
	const int subx = vc & 15, suby = vr & 15, ix = vc >> 4, iy = vr >> 4;
	const int ye = y + iy + h + (suby ? 4 : 0);   /* rows read, exclusive end (the pixel SPU's plan_win) */
	const int ox = plane == 2 ? g->aw / 2 : 0, my = plane ? RSXMC_MID_Y : 0;  /* the plane in the middle surface */
	const int b = plane ? g->border_c : g->border_y;
	const int base = cls * 28;                                                /* the class's lists */
	int32_t *need = plane ? &L->need_c[k] : &L->need_y[k];

	L->sb_row = sb_row;
	if (ye > *need)
		*need = ye;
#ifdef __APPLE__
	{
		const int t = subx && suby ? 2 : subx || suby ? 1 : 0;

		rsxmc_pass[cls][t][0]++;
		rsxmc_pass[cls][t][1] += (unsigned long long)(t == 2 ? w * (2 * h + 7) : w * h);
	}
#endif
	if (!suby)
		put_last(L, g, base + RSXMC_L_H(plane, k), sb_row, plane, cls, x, y, w, h, x + ix + b, y + iy + b, set, subx);
	else if (!subx)
		put_last(L, g, base + RSXMC_L_V(plane, k), sb_row, plane, cls, x, y, w, h, x + ix + b, y + iy + b, set, suby);
	else {
		/* middle row m + j holds the horizontal pass of source row y + iy - 3 + j */
		const int m = RSXMC_MID_ROW(y - sb_row * (plane ? 32 : 64)) + my;

		put(L, base + RSXMC_L_MID(plane, k), x + ox, m, x + w + ox, m + h + 7, ix + b - ox, (y + iy - 3 + b) - m, set, subx);
		put_last(L, g, cls == 0 ? RSXMC_L_VMID : cls == 1 ? RSXMC_L_AVMID : RSXMC_L_BVMID(plane != 0), sb_row, plane, cls, x, y, w, h,
		         x + ox, m + 3, set, suby);
	}
}

void rsxmc_window(rsxmc_lists *L, const rsxmc_geom *g, int sb_row, int plane, int k, int set, int x, int y, int w, int h,
                  int vr, int vc)
{
	window(L, g, sb_row, plane, k, set, x, y, w, h, vr, vc, 0);
}

void rsxmc_window2(rsxmc_lists *L, const rsxmc_geom *g, int sb_row, int plane, int ka, int kb, int set, int x, int y, int w,
                   int h, int vra, int vca, int vrb, int vcb)
{
	window(L, g, sb_row, plane, ka, set, x, y, w, h, vra, vca, 1);
	window(L, g, sb_row, plane, kb, set, x, y, w, h, vrb, vcb, 2);
}

static int round_q4(int v)
{
	return (v < 0 ? v - 2 : v + 2) / 4;
}

static uint16_t u16at(const uint8_t *rec, uint32_t pos)
{
	return *(const uint16_t *)(rec + pos);
}

/* where a block's transform data end (the pixel SPU's coef_end) */
static uint32_t coefs_end(const uint8_t *rec, uint32_t bytes, uint32_t coef, int bsize, int tx_y, int mi_row, int mi_col,
                          int mi_rows, int mi_cols)
{
	const int bw = n8w[bsize], bh = n8h[bsize];
	const int to_right = ((mi_cols - bw - mi_col) * 8) * 8, to_bottom = ((mi_rows - bh - mi_row) * 8) * 8;
	unsigned transforms = 0;
	int plane;

	for (plane = 0; plane < 2; plane++) {
		const int tx = plane ? (bsize < BLOCK_8X8 ? TX_4X4 : (tx_y < mtx[uvb[bsize]] ? tx_y : mtx[uvb[bsize]])) : tx_y;
		const int step = 1 << tx;
		const int max_w = ((bw << 1) >> plane) + (to_right >= 0 ? 0 : to_right >> (5 + plane));
		const int max_h = ((bh << 1) >> plane) + (to_bottom >= 0 ? 0 : to_bottom >> (5 + plane));

		if (max_w > 0 && max_h > 0)
			transforms += (((unsigned)(max_h + step - 1) >> tx) * ((unsigned)(max_w + step - 1) >> tx)) << plane;
	}
	while (transforms--) {
		if (coef + 4 > bytes)
			return 0;
		__builtin_prefetch(rec + coef + 512);
		coef += u16at(rec, coef) ? 4 + 4 * u16at(rec, coef + 2) : 2;
	}
	return (coef + 15) & ~15u;
}

#ifdef __APPLE__
#include <stdio.h>
int rsxmc_dbg_row = -1, rsxmc_dbg_cols = 0;
/* (the Mac model: per block size the inter blocks with one reference, their rectangles, and those with two) */
unsigned long long rsxmc_stat[BLOCK_SIZES][3];
#endif

uint32_t rsxmc_build_row(rsxmc_lists *L, const rsxmc_geom *g, const uint8_t *rec, uint32_t bytes, uint32_t pos, int mi_rows,
                         int mi_cols, int sb_row)
{
	uint32_t mark = pos;                    /* where the markers before the current block start */
	int in_marks = 0;

	while (pos + 16 <= bytes) {
		const vp9_rec_block *b = (const vp9_rec_block *)(rec + pos);
		const unsigned r = b->mi_row, c = b->mi_col, tf = b->type_flags;
		const int bsize = (int)(tf >> 8), inter = (tf & VP9_RB_INTER) != 0;
		const int rf0 = (int8_t)(b->refs >> 8), rf1 = (int8_t)(b->refs & 0xFF), compound = rf1 > INTRA_FRAME;
		uint32_t coef = pos + 16;
		const int16_t *mvq0 = NULL;

		__builtin_prefetch(rec + pos + 512);    /* (the PPU has no prefetcher of its own: the walk ahead) */
		__builtin_prefetch(rec + pos + 640);
		if ((r == VP9_REC_TILE_HI && c == VP9_REC_TILE_LO) || (r == VP9_REC_END && c == VP9_REC_END)) {
			if (!in_marks) {
				mark = pos;
				in_marks = 1;
			}
			pos += 16;
			continue;
		}
		if (r >= (unsigned)mi_rows || c >= (unsigned)mi_cols || bsize >= BLOCK_SIZES) {
			L->failed = 1;
			return bytes;
		}
		if ((int)(r >> 3) != sb_row)
			return in_marks ? mark : pos;   /* the next row's blocks (with the tile record before them) */
		in_marks = 0;
		if (inter) {                        /* the vectors (the pixel SPU's rec_read) */
			if (bsize < BLOCK_8X8) {
				mvq0 = (const int16_t *)(rec + pos + 16);
				coef = pos + (compound ? 48 : 32);
			} else {
				coef = pos + (compound ? 32 : 16);
			}
		}
#ifdef __APPLE__
		if (rsxmc_dbg_row == sb_row && (int)(c >> 3) <= rsxmc_dbg_cols)   /* (the Mac model: a look at a row's blocks) */
			printf("  blk mi %u,%u (px %u,%u) size %d %s refs %d %d mv %d,%d filter %d coefs %d\n", r, c, c * 8, r * 8, bsize,
			       inter ? (compound ? "COMPOUND" : "inter") : "intra", rf0, rf1, inter ? (int16_t)b->x[0] : 0,
			       inter ? (int16_t)b->x[1] : 0, (b->tx_seg >> 4) & 3, (tf & VP9_RB_COEFS) != 0);
#endif
		L->blocks++;
		L->sb_row = sb_row;
		if (inter && compound && g->compound && !g->strips &&
		    ((g->sizes2 ? g->sizes2 : g->sizes ? g->sizes : RSXMC_SIZES_ALL) >> bsize & 1)) {
			/* both predictions (the pixel SPU's plan_block: reference refs >> 8 with mv[0], refs & 0xFF with mv[1]) */
			const int ka0 = rf0 - LAST_FRAME, ka = ka0 < 0 ? 0 : ka0 > 2 ? 2 : ka0;
			const int kb0 = rf1 - LAST_FRAME, kb = kb0 < 0 ? 0 : kb0 > 2 ? 2 : kb0;
			const int set = (b->tx_seg >> 4) & 3, x = (int)c * 8, y = (int)r * 8;

			L->inter++;
			L->compound_rsx++;
			if (bsize >= BLOCK_8X8) {
				const int16_t *m1 = (const int16_t *)(rec + pos + 16);
				const int w = n8w[bsize] * 8, h = n8h[bsize] * 8, ar = (int16_t)b->x[0], ac = (int16_t)b->x[1], br = m1[0], bc = m1[1];

				rsxmc_window2(L, g, sb_row, 0, ka, kb, set, x, y, w, h, ar * 2, ac * 2, br * 2, bc * 2);
				rsxmc_window2(L, g, sb_row, 1, ka, kb, set, x >> 1, y >> 1, w >> 1, h >> 1, ar, ac, br, bc);
				rsxmc_window2(L, g, sb_row, 2, ka, kb, set, x >> 1, y >> 1, w >> 1, h >> 1, ar, ac, br, bc);
			} else {
				const int16_t *m0 = (const int16_t *)(rec + pos + 16), *m1 = (const int16_t *)(rec + pos + 32);
				const int ar = (int16_t)round_q4(m0[0] + m0[2] + m0[4] + m0[6]), ac = (int16_t)round_q4(m0[1] + m0[3] + m0[5] + m0[7]);
				const int br = (int16_t)round_q4(m1[0] + m1[2] + m1[4] + m1[6]), bc = (int16_t)round_q4(m1[1] + m1[3] + m1[5] + m1[7]);
				int i;

				for (i = 0; i < 4; i++)
					rsxmc_window2(L, g, sb_row, 0, ka, kb, set, x + (i & 1) * 4, y + (i >> 1) * 4, 4, 4, m0[2 * i] * 2, m0[2 * i + 1] * 2,
					              m1[2 * i] * 2, m1[2 * i + 1] * 2);
				rsxmc_window2(L, g, sb_row, 1, ka, kb, set, x >> 1, y >> 1, 4, 4, ar, ac, br, bc);
				rsxmc_window2(L, g, sb_row, 2, ka, kb, set, x >> 1, y >> 1, 4, 4, ar, ac, br, bc);
			}
		} else if (inter && compound) {
			L->inter++;
			L->compound++;
#ifdef __APPLE__
			rsxmc_stat[bsize][2]++;
#endif
		} else if (inter && !((g->sizes ? g->sizes : RSXMC_SIZES_ALL) >> bsize & 1)) {
			L->inter++;
			L->small++;                     /* too small to be worth a rectangle: the pixel SPU's */
			L->cls[bsize < BLOCK_16X16 ? 0 : bsize < BLOCK_32X32 ? 1 : 2]++;
		} else if (inter) {
			const int k0 = rf0 - LAST_FRAME, k = k0 < 0 ? 0 : k0 > 2 ? 2 : k0;
			const int set = (b->tx_seg >> 4) & 3, x = (int)c * 8, y = (int)r * 8;
#ifdef __APPLE__
			const uint32_t rects0 = L->rects;
#endif

			L->inter++;
			if (bsize >= BLOCK_8X8) {
				const int w = n8w[bsize] * 8, h = n8h[bsize] * 8, mr = (int16_t)b->x[0], mc = (int16_t)b->x[1];

				rsxmc_window(L, g, sb_row, 0, k, set, x, y, w, h, mr * 2, mc * 2);
				rsxmc_window(L, g, sb_row, 1, k, set, x >> 1, y >> 1, w >> 1, h >> 1, mr, mc);
				rsxmc_window(L, g, sb_row, 2, k, set, x >> 1, y >> 1, w >> 1, h >> 1, mr, mc);
			} else {
				const int mr = (int16_t)round_q4(mvq0[0] + mvq0[2] + mvq0[4] + mvq0[6]);
				const int mc = (int16_t)round_q4(mvq0[1] + mvq0[3] + mvq0[5] + mvq0[7]);
				int i;

				for (i = 0; i < 4; i++)
					rsxmc_window(L, g, sb_row, 0, k, set, x + (i & 1) * 4, y + (i >> 1) * 4, 4, 4, mvq0[2 * i] * 2,
					             mvq0[2 * i + 1] * 2);
				rsxmc_window(L, g, sb_row, 1, k, set, x >> 1, y >> 1, 4, 4, mr, mc);
				rsxmc_window(L, g, sb_row, 2, k, set, x >> 1, y >> 1, 4, 4, mr, mc);
			}
			L->cls[bsize < BLOCK_16X16 ? 0 : bsize < BLOCK_32X32 ? 1 : 2]++;
#ifdef __APPLE__
			rsxmc_stat[bsize][0]++;
			rsxmc_stat[bsize][1] += L->rects - rects0;
#endif
		}
		if (tf & VP9_RB_COEFS) {
			coef = coefs_end(rec, bytes, coef, bsize, (b->tx_seg >> 8) & 3, (int)r, (int)c, mi_rows, mi_cols);
			if (!coef) {
				L->failed = 1;
				return bytes;
			}
		} else {
			coef = (coef + 15) & ~15u;
		}
		if (coef <= pos || coef > bytes) {
			L->failed = 1;
			return bytes;
		}
		pos = coef;
	}
	return bytes;
}

/* ---- the RSX's passes in C ---------------------------------------------------------------------------------- */

static int round7(int s)
{
	s = (s + 64) >> 7;
	return s < 0 ? 0 : s > 255 ? 255 : s;
}

static int tex_at(const rsxmc_tex *t, int x, int y)
{
	x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x;
	y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
	return t->p[(size_t)y * (size_t)t->pitch + (size_t)x];
}

/* an average with the store surface's pixel at the target pixel + (0, dy): (a + b + 1) >> 1 (libvpx's convolve_avg) */
typedef struct {
	const uint8_t *p;                       /* NULL: none */
	int pitch, dy;
} avg_src;

static void pass(const rsxmc_rect *r, uint32_t n, const rsxmc_tex *src, uint8_t *dst, int dpitch, int vertical, const avg_src *a)
{
	uint32_t i;
	int x, y, k;

	for (i = 0; i < n; i++, r++) {
		const int16_t *taps = rsxmc_kernel(r->set, r->pos);

		for (y = r->y0; y < r->y1; y++)
			for (x = r->x0; x < r->x1; x++) {
				int s = 0;

				for (k = 0; k < 8; k++)
					s += taps[k] * (vertical ? tex_at(src, x + r->dx, y + r->dy - 3 + k) : tex_at(src, x + r->dx - 3 + k, y + r->dy));
				s = round7(s);
				if (a && a->p)
					s = (s + a->p[(size_t)(y + a->dy) * (size_t)a->pitch + (size_t)x] + 1) >> 1;
				dst[(size_t)y * (size_t)dpitch + (size_t)x] = (uint8_t)s;
			}
	}
}

void rsxmc_emulate(const rsxmc_lists *L, const rsxmc_geom *g, const rsxmc_tex ref[3][3], uint8_t *pred, int pred_pitch,
                   uint8_t *mid, int mid_pitch, uint8_t *store, int store_pitch)
{
	const rsxmc_tex mt = { mid, g->aw, RSXMC_MID_H, mid_pitch };
	int p, k;

	/* the middle passes of the blocks with one reference and of the compound blocks' first predictions */
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++) {
			pass(L->r[RSXMC_L_MID(p, k)], L->n[RSXMC_L_MID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
			pass(L->r[RSXMC_L_AMID(p, k)], L->n[RSXMC_L_AMID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
		}
	/* the first predictions into the store surface */
	if (store) {
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++) {
				pass(L->r[RSXMC_L_AH(p, k)], L->n[RSXMC_L_AH(p, k)], &ref[k][p], store, store_pitch, 0, NULL);
				pass(L->r[RSXMC_L_AV(p, k)], L->n[RSXMC_L_AV(p, k)], &ref[k][p], store, store_pitch, 1, NULL);
			}
		pass(L->r[RSXMC_L_AVMID], L->n[RSXMC_L_AVMID], &mt, store, store_pitch, 1, NULL);
	}
	/* the blocks with one reference */
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++)
			pass(L->r[RSXMC_L_H(p, k)], L->n[RSXMC_L_H(p, k)], &ref[k][p], pred, pred_pitch, 0, NULL);
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++)
			pass(L->r[RSXMC_L_V(p, k)], L->n[RSXMC_L_V(p, k)], &ref[k][p], pred, pred_pitch, 1, NULL);
	pass(L->r[RSXMC_L_VMID], L->n[RSXMC_L_VMID], &mt, pred, pred_pitch, 1, NULL);
	/* the second predictions: their middle passes (over the first ones'), then the last passes with the average */
	if (store) {
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++)
				pass(L->r[RSXMC_L_BMID(p, k)], L->n[RSXMC_L_BMID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
		for (p = 0; p < 3; p++) {
			const avg_src a = { store, store_pitch, RSXMC_STORE_DY(g, p, L->sb_row) };

			for (k = 0; k < 3; k++) {
				pass(L->r[RSXMC_L_BH(p, k)], L->n[RSXMC_L_BH(p, k)], &ref[k][p], pred, pred_pitch, 0, &a);
				pass(L->r[RSXMC_L_BV(p, k)], L->n[RSXMC_L_BV(p, k)], &ref[k][p], pred, pred_pitch, 1, &a);
			}
		}
		for (p = 0; p < 2; p++) {
			const avg_src a = { store, store_pitch, RSXMC_STORE_DY(g, p, L->sb_row) };

			pass(L->r[RSXMC_L_BVMID(p)], L->n[RSXMC_L_BVMID(p)], &mt, pred, pred_pitch, 1, &a);
		}
	}
}

static uint32_t pass_cmp(const rsxmc_rect *r, uint32_t n, const rsxmc_tex *src, const uint8_t *dst, int dpitch, int vertical,
                         int list, rsxmc_diff *d, uint32_t bad, const avg_src *a)
{
	uint32_t i;
	int x, y, k;

	for (i = 0; i < n; i++, r++) {
		const int16_t *taps = rsxmc_kernel(r->set, r->pos);
		const uint32_t before = bad;

		for (y = r->y0; y < r->y1; y++)
			for (x = r->x0; x < r->x1; x++) {
				const int got = dst[(size_t)y * (size_t)dpitch + (size_t)x];
				int s = 0;

				for (k = 0; k < 8; k++)
					s += taps[k] * (vertical ? tex_at(src, x + r->dx, y + r->dy - 3 + k) : tex_at(src, x + r->dx - 3 + k, y + r->dy));
				s = round7(s);
				if (a && a->p)
					s = (s + a->p[(size_t)(y + a->dy) * (size_t)a->pitch + (size_t)x] + 1) >> 1;
				if (s != got) {
					if (!bad && !d->rects_bad) {
						d->list = list;
						d->index = i;
						d->x = x;
						d->y = y;
						d->got = got;
						d->want = s;
						d->rect = *r;
					}
					bad++;
				}
			}
		d->rects_bad += bad != before;
	}
	return bad;
}

uint32_t rsxmc_compare(const rsxmc_lists *L, const rsxmc_geom *g, const rsxmc_tex ref[3][3], const uint8_t *pred, int pred_pitch,
                       uint8_t *mid, int mid_pitch, uint8_t *store, int store_pitch, rsxmc_diff *d)
{
	const rsxmc_tex mt = { mid, g->aw, RSXMC_MID_H, mid_pitch };
	uint32_t bad = 0;
	int p, k;

	memset(d, 0, sizeof(*d));
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++) {
			pass(L->r[RSXMC_L_MID(p, k)], L->n[RSXMC_L_MID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
			pass(L->r[RSXMC_L_AMID(p, k)], L->n[RSXMC_L_AMID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
		}
	if (store) {
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++) {
				pass(L->r[RSXMC_L_AH(p, k)], L->n[RSXMC_L_AH(p, k)], &ref[k][p], store, store_pitch, 0, NULL);
				pass(L->r[RSXMC_L_AV(p, k)], L->n[RSXMC_L_AV(p, k)], &ref[k][p], store, store_pitch, 1, NULL);
			}
		pass(L->r[RSXMC_L_AVMID], L->n[RSXMC_L_AVMID], &mt, store, store_pitch, 1, NULL);
	}
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++)
			bad = pass_cmp(L->r[RSXMC_L_H(p, k)], L->n[RSXMC_L_H(p, k)], &ref[k][p], pred, pred_pitch, 0, RSXMC_L_H(p, k), d, bad, NULL);
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++)
			bad = pass_cmp(L->r[RSXMC_L_V(p, k)], L->n[RSXMC_L_V(p, k)], &ref[k][p], pred, pred_pitch, 1, RSXMC_L_V(p, k), d, bad, NULL);
	bad = pass_cmp(L->r[RSXMC_L_VMID], L->n[RSXMC_L_VMID], &mt, pred, pred_pitch, 1, RSXMC_L_VMID, d, bad, NULL);
	if (store) {
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++)
				pass(L->r[RSXMC_L_BMID(p, k)], L->n[RSXMC_L_BMID(p, k)], &ref[k][p], mid, mid_pitch, 0, NULL);
		for (p = 0; p < 3; p++) {
			const avg_src a = { store, store_pitch, RSXMC_STORE_DY(g, p, L->sb_row) };

			for (k = 0; k < 3; k++) {
				bad = pass_cmp(L->r[RSXMC_L_BH(p, k)], L->n[RSXMC_L_BH(p, k)], &ref[k][p], pred, pred_pitch, 0, RSXMC_L_BH(p, k), d, bad, &a);
				bad = pass_cmp(L->r[RSXMC_L_BV(p, k)], L->n[RSXMC_L_BV(p, k)], &ref[k][p], pred, pred_pitch, 1, RSXMC_L_BV(p, k), d, bad, &a);
			}
		}
		for (p = 0; p < 2; p++) {
			const avg_src a = { store, store_pitch, RSXMC_STORE_DY(g, p, L->sb_row) };

			bad = pass_cmp(L->r[RSXMC_L_BVMID(p)], L->n[RSXMC_L_BVMID(p)], &mt, pred, pred_pitch, 1, RSXMC_L_BVMID(p), d, bad, &a);
		}
	}
	return bad;
}

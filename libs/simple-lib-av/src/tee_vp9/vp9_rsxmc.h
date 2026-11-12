/* TEE VP9 building block - made by tools/make_baustein.sh from common/vp9_rsxmc.h; do not edit here: change the
   project's file and run the script again. */
/* RSX motion compensation (results/RSX-Einbau-Plan-2026-11-10.md): a superblock row's inter predictions as rectangles
   of 8-tap passes for the RSX, built from the records the pixel stage reads (superblock-row order). One rectangle =
   one 8-tap pass over a destination rectangle of the target surface; the source is the destination moved by an integer
   (dx, dy) in the texture; the taps come per rectangle (the W2 program). The arithmetic is the pixel SPU's plan_block /
   mc_run, and libvpx's convolution:
     integer vector         -> one horizontal pass with the identity kernel (a copy)
     sub-pel only in x      -> one horizontal pass
     sub-pel only in y      -> one vertical pass
     sub-pel in x and y     -> a horizontal pass over the block's rows - 3 .. h + 3 into the middle surface, then a
                               vertical pass from it
   Blocks with two references (compound), with rsxmc_geom.compound (3.47): the first reference's prediction goes into
   the store surface instead, with the same passes; the second one's last pass then reads it there and writes
   (a + b + 1) >> 1 into the prediction surface (the W2A program). Without rsxmc_geom.compound they are left out: the
   pixel SPU predicts them itself.

   Surfaces (all 8 bits a pixel, aw x ah the luma size aligned to 64):
     prediction, aw x (ah + ah / 2):  Y at (0, 0), U at (0, ah), V at (aw / 2, ah). (3.44 tried one surface a
                                      superblock row, 64 wide, a superblock's bytes in one piece for a single transfer
                                      to the pixel SPU: bit-exact, but the RSX took twice as long per rectangle into
                                      such narrow surfaces - see ps3/versions/3.44-packed-surface)
     middle, aw x RSXMC_MID_H:        one superblock row at a time. Y at (0, 0), U at (0, RSXMC_MID_Y), V at
                                      (aw / 2, RSXMC_MID_Y). A block keeps h + 7 rows of its own: at row y of its
                                      superblock row (a multiple of 4) from RSXMC_MID_ROW(y) on
     store, aw x RSXMC_STORE_H:       one superblock row at a time, the first predictions of its compound blocks:
                                      as the row's band of the prediction surface. Y at (0, 0), U at (0, 64), V at
                                      (aw / 2, 64)
     reference textures:              a frame buffer's plane with its border, stride x (plane height + 2 borders)

   rsxmc_geom.packed (3.49, the 3.44 trial again as an option): the prediction is one surface a superblock row, 64 wide
   (pitch 64) and aw / 64 * 96 high: superblock column c has rows c * 96 ..: 64 rows of luma, then 32 rows with U in
   bytes 0..31 and V in bytes 32..63. A superblock's predictions are RSXMC_SB_BYTES in one piece, which a pixel SPU
   fetches with one transfer instead of a list of 128 elements. A frame's rows lie behind each other: superblock
   (r, c) at (r * aw / 64 + c) * RSXMC_SB_BYTES. The store surface has the same shape then (the same bytes read as
   64 wide), a block's first prediction at its prediction's place. Measured on the PS3 (probes 4.8 - 4.12): the RSX
   is slow at this. Scattered small rectangles into main memory cost it 0.2 us more for every further pixel row of
   the rectangle within the same 512 bytes, and pitches that are powers of two cost more again; a copy out of RSX
   memory into such a surface is as slow.

   rsxmc_geom.strips (3.49): the prediction surface is RSXMC_STRIP_PITCH wide and holds a cell of 192 x 32 for every
   superblock, RSXMC_STRIP_PER cells side by side, superblock n = sb_row * (aw / 64) + sb_col at
   ((n % PER) * 192, (n / PER) * 32). A cell's 32 rows: the superblock's luma rows 0..31 in bytes 0..63, its luma rows
   32..63 in bytes 64..127, U in bytes 128..159, V in bytes 160..191. A pixel SPU fetches a superblock with a list of
   32 elements of 192 bytes (128 elements for the frame-shaped surface: the MFC takes some 80 ns an element, and the
   pixel SPUs' other transfers wait meanwhile). A rectangle's pixel rows stay a whole pitch apart in memory, which is
   what the RSX wants (probe 4.13: 0.22 us for a scattered 8 x 8 rectangle, 0.24 frame-shaped). Luma blocks 64 high
   become two rectangles. Blocks with two references are not drawn in this layout (the pixel SPU predicts them). */
#ifndef VP9_RSXMC_H
#define VP9_RSXMC_H

#include <stdint.h>

#define RSXMC_MID_ROW(y)   ((y) + 7 * ((y) >> 2))
#define RSXMC_MID_Y        184              /* luma rows of the middle surface (a 4-row block at row 60: rows 165..175) */
#define RSXMC_MID_C        96               /* chroma rows (a 4-row block at row 28: rows 77..87) */
#define RSXMC_MID_H        (RSXMC_MID_Y + RSXMC_MID_C)

#define RSXMC_STORE_H      96

#define RSXMC_NLIST        85
#define RSXMC_L_MID(p, k)  ((p) * 3 + (k))        /* horizontal: reference k's plane p -> middle */
#define RSXMC_L_H(p, k)    (9 + (p) * 3 + (k))    /* horizontal: reference -> prediction */
#define RSXMC_L_V(p, k)    (18 + (p) * 3 + (k))   /* vertical: reference -> prediction */
#define RSXMC_L_VMID       27                     /* vertical: middle -> prediction (all planes, all references) */
/* compound blocks, their first prediction: the same passes, the last one into the store surface */
#define RSXMC_L_AMID(p, k) (28 + (p) * 3 + (k))
#define RSXMC_L_AH(p, k)   (37 + (p) * 3 + (k))
#define RSXMC_L_AV(p, k)   (46 + (p) * 3 + (k))
#define RSXMC_L_AVMID      55
/* ... their second prediction: the last pass averages with the store's pixel. Its middle passes are drawn after the
   first prediction's last ones (they use the same rows of the middle surface) */
#define RSXMC_L_BMID(p, k) (56 + (p) * 3 + (k))
#define RSXMC_L_BH(p, k)   (65 + (p) * 3 + (k))
#define RSXMC_L_BV(p, k)   (74 + (p) * 3 + (k))
#define RSXMC_L_BVMID(c)   (83 + (c))             /* middle -> prediction: 0 luma, 1 chroma (their store offsets differ) */
/* the store's pixel for a prediction pixel of plane p in superblock row r: prediction position + (0, this) */
#define RSXMC_STORE_DY(g, p, r) ((g)->packed ? 0 : (p) ? 64 - 32 * (r) - (g)->ah : -64 * (r))

#define RSXMC_STRIP_W      192              /* strips: a superblock's cell */
#define RSXMC_STRIP_H      32
#define RSXMC_STRIP_PER    12               /* ... cells side by side */
#define RSXMC_STRIP_PITCH  (RSXMC_STRIP_PER * RSXMC_STRIP_W)            /* 2304 = 9 * 256: no power of two */
#define RSXMC_STRIP_ROWS(g)  ((((g)->aw / 64) * ((g)->ah / 64) + RSXMC_STRIP_PER - 1) / RSXMC_STRIP_PER * RSXMC_STRIP_H)
#define RSXMC_STRIP_BYTES(g) ((size_t)RSXMC_STRIP_ROWS(g) * RSXMC_STRIP_PITCH)

#define RSXMC_SB_BYTES     6144             /* packed: a superblock's predictions (64 x 64 + 32 x (32 + 32)) */
#define RSXMC_ROW_H(g)     ((g)->aw / 64 * 96)                          /* ... a superblock row's surface: its height */
#define RSXMC_ROW_BYTES(g) ((size_t)((g)->aw / 64) * RSXMC_SB_BYTES)    /* ... its bytes */

typedef struct {
	int16_t x0, y0, x1, y1;                 /* destination: [x0, x1) x [y0, y1) of the target surface */
	int16_t dx, dy;                         /* source texel = destination pixel + (dx, dy) */
	uint8_t set, pos;                       /* the taps: rsxmc_kernel(set, pos) (position 0: the copy kernel) */
	uint8_t pad[2];
} rsxmc_rect;                               /* 16 bytes */

typedef struct {
	int aw, ah;                             /* luma size aligned to 64 */
	int border_y, border_c;                 /* the reference planes' borders (PIX_BORDER_Y, PIX_BORDER_C) */
	uint32_t sizes;                         /* bit b: blocks of size index b (BLOCK_4X4 = 0 .. BLOCK_64X64 = 12) get
	                                           rectangles; 0: all of them. The other inter blocks are the pixel SPU's
	                                           (pix_job.rsx_sizes must say the same) */
	int compound;                           /* blocks with two references get rectangles too (PIXJ_RSX_COMPOUND) */
	uint32_t sizes2;                        /* ... those of these sizes (as `sizes`); 0: the sizes of `sizes` */
	int packed;                             /* the prediction surface a superblock row, 64 wide (see above) */
	int strips;                             /* the prediction surface in cells of 192 x 32 (see above); before `packed` */
} rsxmc_geom;

#define RSXMC_SIZES_ALL    0x1FFFu

typedef struct {
	rsxmc_rect *r[RSXMC_NLIST];
	uint32_t n[RSXMC_NLIST], cap[RSXMC_NLIST];
	int32_t need_y[3], need_c[3];           /* per reference: luma / chroma rows read, exclusive end (the pixel
	                                           stage's pix_prog.lim_y / lim_c must reach them before the row is drawn) */
	int sb_row;                             /* the superblock row the lists are for */
	uint32_t blocks, inter, compound, rects;   /* (compound: blocks with two references left to the pixel SPU) */
	uint32_t compound_rsx;                  /* ... those with rectangles */
	uint32_t small;                         /* inter blocks with one reference left to the pixel SPU (rsxmc_geom.sizes) */
	uint32_t cls[3];                        /* all inter blocks with one reference, whoever predicts them: smaller than
	                                           16x16, from 16x16 on, from 32x32 on (what a frame would cost the RSX) */
	int failed;                             /* a list ran out of memory, or the records did not parse */
} rsxmc_lists;

/* the lists empty again (the buffers stay) */
void rsxmc_reset(rsxmc_lists *L);
void rsxmc_free(rsxmc_lists *L);

/* one w x h prediction at (x, y) of plane `plane` (its own pixels) in superblock row sb_row, from reference k, filter
   set `set`, vector (vr, vc) in 1/16 of the plane's pixel: its rectangles appended to L */
void rsxmc_window(rsxmc_lists *L, const rsxmc_geom *g, int sb_row, int plane, int k, int set, int x, int y, int w, int h,
                  int vr, int vc);

/* the inter blocks of superblock row sb_row from the records rec (bytes) at pos on, appended to L; stops at the first
   block of another row. Returns the position to go on from (bytes at the end). L->failed on a parse error. */
uint32_t rsxmc_build_row(rsxmc_lists *L, const rsxmc_geom *g, const uint8_t *rec, uint32_t bytes, uint32_t pos, int mi_rows,
                         int mi_cols, int sb_row);

/* the 8 taps of filter set s (0 regular, 1 smooth, 2 sharp, 3 bilinear) at sub-pel position p */
const int16_t *rsxmc_kernel(int s, int p);

/* ---- the RSX's passes in C (what the W2 program computes: bit-exact on the console, RSX probe 2.0) ----------- */

typedef struct {
	const uint8_t *p;                       /* the texture's first byte */
	int w, h, pitch;                        /* reads are clamped to [0, w) x [0, h) */
} rsxmc_tex;

/* a row's lists drawn in the RSX's order: the middle passes, then the direct ones, then middle -> prediction.
   ref[k][plane]: the reference planes with their borders; pred (aw wide, pitch pred_pitch) and mid (aw x RSXMC_MID_H,
   pitch mid_pitch): the surfaces */
void rsxmc_emulate(const rsxmc_lists *L, const rsxmc_geom *g, const rsxmc_tex ref[3][3], uint8_t *pred, int pred_pitch,
                   uint8_t *mid, int mid_pitch, uint8_t *store, int store_pitch);

/* the same passes, but those into the prediction surface compared with what it holds (pred) instead of written.
   Returns the bytes that differ; *d: the first one's list, rectangle, place and values, and how many rectangles differ */
typedef struct {
	int list, x, y, got, want;
	uint32_t index, rects_bad;
	rsxmc_rect rect;
} rsxmc_diff;

uint32_t rsxmc_compare(const rsxmc_lists *L, const rsxmc_geom *g, const rsxmc_tex ref[3][3], const uint8_t *pred, int pred_pitch,
                       uint8_t *mid, int mid_pitch, uint8_t *store, int store_pitch, rsxmc_diff *d);

/* a compound block's prediction (both references) as rsxmc_window: reference ka with vector (vra, vca), kb with
   (vrb, vcb) */
void rsxmc_window2(rsxmc_lists *L, const rsxmc_geom *g, int sb_row, int plane, int ka, int kb, int set, int x, int y, int w,
                   int h, int vra, int vca, int vrb, int vcb);

#endif

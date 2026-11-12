/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/rsx_mc.h; do not edit here: change the
   project's file and run the script again. */
/* The RSX's inter prediction for the pixel jobs (results/RSX-Einbau-Plan-2026-11-10.md, step E3): superblock row by
   superblock row the rectangles of common/vp9_rsxmc.c are drawn by the W2 program (rsx-probe: bit-exact on the
   console) from the reference frames into a prediction surface in main memory, which the pixel SPUs read
   (PIXJ_RSX_PRED). A PPU thread of its own lets the RSX draw a row as soon as the reference rows it reads are final,
   and the RSX counts the rows done in a label the pixel SPUs wait on. Shares the display's gcm context (gfx.c). */
#ifndef RSX_MC_H
#define RSX_MC_H

#include <stddef.h>
#include <stdint.h>

#include "vp9_pix_job.h"
#include "vp9_rsxmc.h"

#define RSX_MC_SLOTS 3                      /* frames whose predictions are under way or in use at a time */

/* fills L (reset before the call) with superblock row sb_row's rectangles */
typedef void (*rsx_mc_rowfn)(void *user, int sb_row, rsxmc_lists *L);

typedef struct {
	pix_frame ref[3];                       /* the references' frames (their top-left visible samples) */
	const pix_prog *prog[3];                /* ... their progress (NULL: complete) */
	uint32_t stride_y, stride_c;
	int sb_rows;
	int order;                              /* decode order: the earlier frame's rows go first */
	rsx_mc_rowfn fn;
	void *user;
} rsx_mc_desc;

/* the prediction surfaces for frames up to aw x ah, allocated now (before the streams cut up the heap) and kept */
int rsx_mc_prealloc(int aw, int ah);
/* from the next rsx_mc_init on: vertices of 16 bytes (16-bit coordinates, 8-bit taps) instead of 48 (floats) */
void rsx_mc_compact(int on);
/* 1: a row's label is written after a switch of the render target (3.42d: that does not keep single pixel rows from
   arriving after the label; kept for comparisons) */
void rsx_mc_row_flush(int on);
/* the block sizes that get rectangles (rsxmc_geom.sizes: bit = size index, 0 = all); to be set between runs, and the
   pixel jobs told the same (pix_job.rsx_sizes) */
void rsx_mc_sizes(uint32_t mask);
/* 1: blocks with two references get rectangles too (rsxmc_geom.compound); from the next rsx_mc_start on. The pixel
   jobs must be told the same (PIXJ_RSX_COMPOUND) */
void rsx_mc_compound(int on);
/* the fragment programs, from the next rsx_mc_start on: 0 W2 / W2A (texels and taps snapped to integers: 22 / 30
   instructions); 1 W3 / W3A (nothing snapped, 14 / 19: NOT exact on the RSX, probe 4.5); 2 W3T / W3TA (texels snapped,
   18 / 23); 3 W3K / W2A (taps snapped, 18 / 30) */
void rsx_mc_short(int on);
/* 1: compact vertices are drawn as quads (4 vertices a rectangle, no index buffer) instead of two triangles */
void rsx_mc_quads(int on);
/* 1 or 2: the packed prediction surface (rsxmc_geom.packed: a superblock row 64 wide, a superblock's 6144 bytes in one
   piece); from the next rsx_mc_init / rsx_mc_start on. 1: drawn directly into main memory (slow on the RSX); 2: drawn
   in RSX memory and copied into main memory a superblock row at a time (what a row did not predict is undefined then). rsx_mc_begin then gives pitch 64 and the slot's first byte
   as pred->y: superblock (r, c) lies at (r * aw / 64 + c) * RSXMC_SB_BYTES */
void rsx_mc_packed(int on);
/* 1: the prediction surface in cells of 192 x 32 a superblock (rsxmc_geom.strips, PIXJ_RSX_STRIPS): rsx_mc_begin gives
   pitch RSXMC_STRIP_PITCH and the slot's first byte as pred->y. Blocks with two references stay the pixel SPUs' then,
   whatever rsx_mc_compound says. From the next rsx_mc_init / rsx_mc_start on */
void rsx_mc_strips(int on);
/* 0: the RSX does not wait for its back end (label + wait) before it reads a surface it has just drawn; 1 (default): it does */
void rsx_mc_sync(int on);
#ifdef RSX_MC_BENCH
/* the probe: what the RSX's steps cost (target switches, syncs, texture sets, pixels by rectangle size), into the log.
   After rsx_mc_init with compact vertices and compound blocks asked for, before rsx_mc_start */
void rsx_mc_bench(void);
/* ... the RSX's time for a frame's first rows as the decoder draws them, frame-shaped against packed (d: the frame as
   for rsx_mc_begin). rsx_mc_bench_filter: only some lists (bit 0 two-pass, bit 1 one-pass) and planes (bit p) */
void rsx_mc_bench_rows(const rsx_mc_desc *d, int rows, const char *what);
void rsx_mc_bench_filter(unsigned lists, unsigned planes);
#endif
/* 1: after every row the RSX also draws a marker into main memory (the pixel SPUs wait for it: PIXJ_RSX_MARKER) */
void rsx_mc_row_marker(int on);
/* a slot's marker for its frame begun last: where it is, and the three bits its bytes carry (pix_job.pred_lab_base) */
void rsx_mc_marker(int slot, uint64_t *ea, uint32_t *gen);
/* the n frame buffer regions (each 1 MB aligned, fb_bytes each, a multiple of 1 MB) mapped for the RSX; surfaces,
   vertices and programs set up for frames of aw x ah (luma, aligned to 64). 0, or -1 (logged): no RSX */
int rsx_mc_init(void *const *fb, int n, size_t fb_bytes, int aw, int ah);
void rsx_mc_release(void);
const rsxmc_geom *rsx_mc_geom(void);

/* a run: the worker thread runs from start to stop; the display keeps quiet meanwhile */
int rsx_mc_start(void);
void rsx_mc_stop(void);
/* something went wrong since start (logged): the jobs waiting for rows will time out */
int rsx_mc_failed(void);

/* a frame's predictions from now on: its slot, or -1 when none is free. The job fields for PIXJ_RSX_PRED come back */
int rsx_mc_begin(const rsx_mc_desc *d, pix_frame *pred, uint32_t *pitch, uint64_t *lab_ea, uint32_t *lab_base);
/* its jobs are done (or given up): the slot is free again */
void rsx_mc_end(int slot);
/* a slot's surface (for a check on the PPU) and the row its label says is done (-1: none) */
const uint8_t *rsx_mc_surface(int slot);
int rsx_mc_rows_done(int slot);
/* what happened while a row was drawn: 1 the command buffer started over, 2 the vertex ring was full, 4 it had
   two-pass blocks, 8 its label was seen within 100 us of its commands */
int rsx_mc_row_flags(int slot, int row);

/* the counts since start into the log */
void rsx_mc_report(const char *name);
/* The RSX part is given up for this stream (it has failed, or a pixel SPE waited for a row in vain): no more rows are
   drawn, and every slot's marker and label say "all rows are there", so that no pixel SPE waits for a row that will
   not come. What the SPEs then read is not a prediction - the caller must not show those frames. To be called again
   while frames with a slot are under way: rows the RSX draws late set a marker back */
void rsx_mc_give_up(void);
/* (tests of the failure paths) for the run that starts next: after fail_rows rows the RSX part fails as it does when
   the RSX stops taking commands; after stall_rows rows the RSX is made to stand still for stall_ms (a wait label the
   PPU lets go again). 0: not */
void rsx_mc_test_inject(int fail_rows, int stall_rows, int stall_ms);
/* (builds with -DRSX_EMIT_CHECK only) the command emitter compared with librsx, made-up arguments; a line in the log */
void rsx_mc_emit_selftest(void);

#endif

/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/rsx_mc.c; do not edit here: change the
   project's file and run the script again. */
/* The RSX's inter prediction for the pixel jobs, superblock row by superblock row (see rsx_mc.h). The drawing follows
   the RSX probe's proven setup (rsx-probe): B8 targets without a depth buffer, viewport B (pixel / 2048 - 1), textures
   B8 linear with unnormalised coordinates, NEAREST, clamped at the plane's edges, the byte in all four channels; before
   every use of a texture its whole state is loaded again (probe 3.3: an invalidation alone leaves stale bytes).

   Per row: [the two-pass blocks' horizontal passes into the middle surface (RSX memory); the RSX waits for its own back
   end] the one-pass blocks and the vertical passes out of the middle surface into the frame's prediction surface (main
   memory); a back-end label counts the row. Vertices: 4 a rectangle (48 bytes each, floats written as bit patterns
   from a table: an int-to-float conversion costs the PPU about 60 cycles), in a ring in the main memory mapped behind
   the command buffer, drawn through one fixed index buffer; a label after every draw gives the ring's space back.
   Commands: the whole command buffer as one piece (gfx.c), started over when its end comes near. */
#include "rsx_mc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (3.71) nothing of an SDK by name in here: the graphics library's few calls through rsx_plat.h (Sony's libgcm or
   PSL1GHT's), threads, time, memory and the log through the decoder's tv9_plat.h, the application's display code
   through tv9_rsx_host.h, the programs as arrays (tv9_rsx_programs.c) */
#include "rsx_plat.h"
#include "tv9_plat.h"
#include "tv9_rsx_host.h"
#include "tv9_rsx_programs.h"

/* the RSX's command words: functions of our own (rsx_emit.h), the same with both SDKs. -DRSX_EMIT_CHECK (PSL1GHT): librsx
   writes them and ours are compared with its words (rsx_emit_check.h) */
#define RE_CTX rxp_ctx
#include "rsx_emit.h"
#ifdef RSX_EMIT_CHECK
#include "rsx_emit_check.h"
#endif

#define log_line tv9p_log                   /* (the decoder's log: the application's, when it gave one) */

/* microseconds, for waits and the statistics */
static inline s64 now_us(void)
{
	return (s64)((double)tv9p_ticks() / tv9p_tick_mhz());
}


#define LAB_CHUNK   200                     /* the draws done (labels 0-63 are the system's) */
#define LAB_SYNC    201                     /* a row's middle passes are written before they are read */
#define LAB_TEST    202                     /* (rsx_mc_test_inject) the RSX waits here until the PPU lets it go */
#define LAB_ROW0    210                     /* + slot: base + the superblock row predicted last */
#define CHUNK_RECTS 4096                    /* rectangles a draw (16-bit indices: 4 vertices each) */
#define ROW_CMD     0xC000                  /* command bytes asked for before a row */
#define DRAW_CMD    1024                    /* ... before a draw */
#define WAIT_US     2000000
#define FT_OFF      4096                    /* the float table: -4096 .. 8191 */
#define FT_N        12288
#define MAX_FB      16
#define RING_BYTES  (3u << 20)              /* the vertex ring */

typedef struct {
	u32 w[12];                              /* x, y (target pixels), u, v (source texels), 8 taps: f32 bit patterns */
} vtx;

#define ROWS_BYTES  (1u << 20)              /* per slot: the rectangles of the rows built and not drawn yet */
#define MAX_ROWS    64

typedef struct {
	u32 start, bytes;                       /* its rectangles in the slot's ring, list after list */
	u32 n[RSXMC_NLIST];
	u32 rects;
	int32_t need_y[3], need_c[3];
} row_meta;

typedef struct {
	const rsxmc_rect *r[RSXMC_NLIST];
	const u32 *n;
	u32 rects;
} row_lists;

typedef struct {
	volatile int state;                     /* 0 free, 1 rows to draw, 2 all drawn (or given up) */
	volatile int abort;
	volatile int building;                  /* the builder thread is at work on it */
	rsx_mc_desc d;
	volatile int row;                       /* the next row to draw */
	volatile int b_row;                     /* the rows built (in the ring): 0 .. b_row - 1 */
	int built;                              /* row b_row's lists are in L, waiting for room in the ring */
	u8 *ring;                               /* ROWS_BYTES */
	u32 r_wr;
	row_meta rows[MAX_ROWS];
	u8 rowflag[MAX_ROWS];                   /* 1: the command buffer started over within the row, 2: the vertex ring was
	                                           full, 4: it had two-pass blocks, 8: its label was seen within 100 us */
	int seen;                               /* the last row whose label was seen (the latency count) */
	s64 t_emit[64];                         /* when each row's commands went out */
	u32 base;
	u32 tex[3][3];                          /* [reference][plane]: the plane with its border, RSX offset */
	rsxmc_lists L;
} mc_slot;

/* (3.64, -DRSX_MC_CAPTURE) the command words every librsx call writes, for the first few different results per call:
   the ground truth for an emitter of our own, which the Sony SDK build needs (it has no librsx) */
#ifdef RSX_MC_CAPTURE
#include <stdio.h>
static void cap_note(const char *name, const u32 *a, const u32 *b)
{
	static struct {
		const char *name;
		u32 hash[8];
		int n;
	} seen[40];
	const int words = (int)(b - a);
	u32 h = 2166136261u;
	int k, i;

	for (i = 0; i < words; i++)
		h = (h ^ a[i]) * 16777619u;
	for (k = 0; k < 40 && seen[k].name && seen[k].name != name; k++)
		;
	if (k == 40)
		return;
	seen[k].name = name;
	for (i = 0; i < seen[k].n; i++)
		if (seen[k].hash[i] == h)
			return;
	if (seen[k].n == 8)
		return;
	seen[k].hash[seen[k].n++] = h;
	for (i = 0; i < words; i += 12) {
		char line[160];
		int o = 0, j;

		for (j = i; j < words && j < i + 12; j++)
			o += snprintf(line + o, sizeof(line) - (size_t)o, " %08x", (unsigned)a[j]);
		log_line("RSXCAP %s #%d %d/%d:%s", name, seen[k].n, i, words, line);
	}
}
#define CAP(name, call) do { const u32 *cap0_ = ctx->current; call; cap_note(name, cap0_, ctx->current); } while (0)
#else
#define CAP(name, call) call
#endif

static rxp_ctx *ctx;
static int ready;
static rsxmc_geom geom;
static void *fb_mapped[MAX_FB];
static int fb_n;
static uint8_t *pred_mem;
static size_t pred_bytes, surf_bytes;
static int pred_mapped;
static u32 pred_off0;
static uint8_t *mid;
static u32 mid_off;
/* the row markers: 64 x 64 bytes at the end of the prediction memory, a line a slot; the RSX draws 16 bytes there after
   a row, copied out of a ramp texture (texel i = i - 16), so that a marker's bytes are v, v + 1, ... */
#define MARK_BYTES  4096
static uint8_t *ramp;
static u32 ramp_off, mark_off;
static u8 slot_gen[RSX_MC_SLOTS];           /* counts a slot's frames: the marker's upper three bits */
static int row_marker = 1;                  /* rsx_mc_row_marker */
static u16 *idx;
static u32 idx_off;
/* the fragment programs: W2 (8 taps), W2A (8 taps and the average with the store's pixel), and shorter forms of them
   (rsx_mc_short): W3 / W3A snap neither texels nor taps to integers (probe 4.5: NOT exact on the RSX), W3T / W3TA
   snap the texels only, W3K the taps only */
#define NFP 8
#define FP_COPY 7                           /* CP: a texel copied (the packed surface's way into main memory) */
static const re_fp *fpx[NFP];
static u32 fpx_off[NFP];
static const u8 fp_pick[4][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 1 } };   /* [rsx_mc_short][the average] */
static int short_want, shortp;              /* rsx_mc_short; in use since rsx_mc_start */
static const re_vp *vp;
static const void *vp_ucode;
static const re_const *vc_scale, *vc_offset, *vc_tap[4], *vc_bias;
/* compound blocks (3.47): their first prediction goes into the store surface (RSX memory, a superblock row's band of
   the prediction surface); the second one's last pass is drawn with the W2A program, which reads the store's pixel
   through texture unit 1 at the target pixel + a constant (firstOfs) and writes (a + b + 1) >> 1 */
static uint8_t *store;
static u32 store_off;
static const re_const *vc_first;
static int two_want, two;                   /* rsx_mc_compound; in use since rsx_mc_start */
static int fp_cur = -1;                     /* the fragment program loaded: index of fpx */
static unsigned state_seen;                 /* tv9h_state_gen() when the programs were loaded last */
static void *fp_mem[NFP];                   /* the programs' code in RSX memory */
static u64 st_two_rows;                     /* rows with compound rectangles */
static int row_flush;                   /* a row's label only after a switch of the render target (rsx_mc_row_flush) */
static int compact_want, compact;           /* vertices of 16 bytes (16-bit coordinates, 8-bit taps + 64; program vp_w2c)
                                               instead of 48 (floats; vp_w2): asked for; in use since rsx_mc_init */
static u64 ktab8[64];                       /* [set * 16 + position]: the 8 taps + 64 as bytes */
static int packed_want;                     /* rsx_mc_packed: geom.packed from the next rsx_mc_init / rsx_mc_start on */
/* The packed surface's rows are written in one of two ways. Directly (rsx_mc_packed 1, as 3.44): the RSX is slow at
   that - a small rectangle's pixel rows lie 64 bytes apart there, and scattered rectangles into main memory cost it
   0.2 us more for every further row within the same 512 bytes (probe 4.10 / 4.11: 1.6 us for an 8 x 8 rectangle
   against 0.24 into the frame-shaped surface, 0.15 into RSX memory). Or (rsx_mc_packed 2): into a surface of the same
   shape in RSX memory (ploc), and from there into main memory with one rectangle a superblock row - dense writes
   in memory order, which cost no more than their pixels */
static int strips_want;                     /* rsx_mc_strips: geom.strips (cells of 192 x 32) from the next init / start on */
static int ploc_use;
static uint8_t *ploc;
static u32 ploc_off;
static int sync_on = 1;                     /* rsx_mc_sync: the RSX waits for its back end before it reads what it drew */
static u64 st_copies;
#ifdef RSX_MC_BENCH
static int bench_noflush;                   /* emit_row leaves its commands unflushed (the measurement hands them over) */
#else
#define bench_noflush 0
#endif
static int quads;                           /* compact vertices drawn as quads (4 vertices, no indices) instead of two
                                               triangles through the index buffer (rsx_mc_quads; the probe's test) */
static u32 ftab[FT_N];
static u64 ktab[64][4];                     /* [set * 16 + position]: the 8 taps as f32 bit patterns, two a word */
static uint8_t *ring;                       /* RING_BYTES, kept from stream to stream as the prediction memory */
static int ring_mapped;
static u32 ring_off, ring_size, ring_wr;
static struct {
	u32 start, end, id;
} fl[256];                                  /* the ring's regions whose draws are not done yet, oldest first */
static int fl_head, fl_n;
static u32 chunk_id, sync_id, lab_gen;
static volatile int failed, stop_flag, running;
static void *thread, *thread_b;                /* tv9p_thread_start's */
static u32 unlabeled;                       /* vertex bytes of draws since the last LAB_CHUNK label */
#define LABEL_EVERY (256u << 10)
static mc_slot slots[RSX_MC_SLOTS];
/* counts since rsx_mc_start */
static u64 st_rows, st_rects, st_frames, st_us_build, st_us_emit, st_us_ring, st_us_cb, st_cmd_bytes, st_vtx_bytes;
static u32 st_restarts, st_ringwaits, st_row_max, st_idle, st_draws;
static u64 st_lat_us, st_lat_n, st_lat_max, st_fly_bytes, st_fly_n, st_fly_max;   /* a row from its commands to its label;
                                                                                    vertex bytes not drawn yet at a row's start */

static void fail(const char *what);

/* the rows whose labels have come since the last look: how long the RSX took from their commands */
static void rows_seen(void)
{
	const s64 now = now_us();
	int i;

	for (i = 0; i < RSX_MC_SLOTS; i++) {
		mc_slot *s = &slots[i];
		int done;

		if (s->state == 0 || s->seen >= s->row - 1)
			continue;
		done = (int)(s32)(*(vu32 *)rxp_label((u8)(LAB_ROW0 + i)) - s->base);
		while (s->seen < done && s->seen < s->row - 1) {
			const u64 us = (u64)(now - s->t_emit[++s->seen & 63]);

			if (us < 100)
				s->rowflag[s->seen & 63] |= 8;
			st_lat_us += us;
			st_lat_n++;
			if (us > st_lat_max)
				st_lat_max = us;
		}
		/* a row whose commands went out two seconds ago and whose label has not come: the RSX is not drawing (the
		   pixel SPEs would wait 4 s for that row before they give up) */
		if (!failed && s->seen < s->row - 1 && now - s->t_emit[(s->seen + 1) & 63] > WAIT_US)
			fail("a row's label did not come within 2 s");
	}
}

static void fail(const char *what)
{
	if (!failed)
		log_line("rsx_mc: FAILED: %s - no more rows from the RSX", what);
	failed = 1;
	__asm__ volatile("sync" ::: "memory");
	tv9p_wake();                            /* (the decoder's thread may be waiting for its SPEs: it looks at once) */
}

static u32 offset_of(const void *p)
{
	u32 o = 0;

	if (rxp_offset(p, &o) != 0)
		log_line("rsx_mc: no RSX offset for %p", p);
	return o;
}

/* PPU caches: written lines out and dropped (before the RSX writes main memory the PPU has written) */
static void cache_flush(const void *p, size_t n)
{
	uintptr_t a;

	for (a = (uintptr_t)p & ~(uintptr_t)127; a < (uintptr_t)p + n; a += 128)
		__asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("sync" ::: "memory");
}

static inline u32 fbits(int v)
{
	const unsigned i = (unsigned)(v + FT_OFF);
	union {
		f32 f;
		u32 u;
	} c;

	if (i < FT_N)
		return ftab[i];
	c.f = (f32)v;
	return c.u;
}

/* ---- render state (as proven by the probe) ------------------------------------------------------------------ */

/* W2's tap offsets for a horizontal (0) or vertical (1) pass */
static void taps(int vertical)
{
	static const f32 h[4][4] = { { -3, 0, -2, 0 }, { -1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 3, 0, 4, 0 } };
	static const f32 v[4][4] = { { 0, -3, 0, -2 }, { 0, -1, 0, 0 }, { 0, 1, 0, 2 }, { 0, 3, 0, 4 } };
	int i;

	for (i = 0; i < 4; i++)
		CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_tap[i], vertical ? v[i] : h[i]));
}

static void fp_use(int avg);

/* the programs, once a run */
static void programs_load(void)
{
	const f32 sc[4] = { 1.0f / 2048.0f, 1.0f / 2048.0f, 0.0f, 0.0f }, of[4] = { -1.0f, -1.0f, 0.0f, 1.0f };

	fp_cur = -1;
	state_seen = tv9h_state_gen();
	fp_use(0);
	CAP("rsxLoadVertexProgram", re_load_vertex_program(ctx, vp, vp_ucode));
	CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_scale, sc));
	CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_offset, of));
	if (compact) {
		const f32 bias[4] = { -64.0f, -64.0f, -64.0f, -64.0f };

		CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_bias, bias));
	}
}

/* a B8 target, no depth buffer, every other state set to what W2 needs */
static void target_set(u8 location, u32 offset, u32 pitch, u16 w, u16 h)
{
	re_surface sf;
	const f32 scale[4] = { 2048.0f, 2048.0f, 0.5f, 0.0f }, offs[4] = { 2048.0f, 2048.0f, 0.5f, 0.0f };
	int i;

	memset(&sf, 0, sizeof(sf));
	sf.type = RE_SURFACE_LINEAR;
	sf.antialias = RE_SURFACE_CENTER_1;
	sf.color_format = RE_SURFACE_B8;
	sf.color_target = RE_SURFACE_TARGET_0;
	sf.color_location[0] = location;
	sf.color_offset[0] = offset;
	sf.color_pitch[0] = pitch;
	for (i = 1; i < RE_MRT; i++) {
		sf.color_location[i] = RE_LOCATION_RSX;
		sf.color_offset[i] = 0;
		sf.color_pitch[i] = 64;
	}
	sf.depth_format = RE_SURFACE_Z24S8;
	sf.depth_location = RE_LOCATION_RSX;
	sf.depth_offset = 0;
	sf.depth_pitch = 64;
	sf.width = w;
	sf.height = h;
	CAP("rsxSetSurface", re_set_surface(ctx, &sf));
	CAP("rsxSetViewport", re_set_viewport(ctx, 0, 0, 4096, 4096, 0.0f, 1.0f, scale, offs));
	CAP("rsxSetScissor", re_set_scissor(ctx, 0, 0, w, h));
	for (i = 0; i < 8; i++)
		CAP("rsxSetViewportClip", re_set_viewport_clip(ctx, (u8)i, w, h));
	CAP("rsxSetDepthTestEnable", re_set_depth_test_enable(ctx, RE_FALSE));
	CAP("rsxSetDepthWriteEnable", re_set_depth_write_enable(ctx, RE_FALSE));
	CAP("rsxSetStencilTestEnable", re_set_stencil_test_enable(ctx, RE_FALSE));
	CAP("rsxSetAlphaTestEnable", re_set_alpha_test_enable(ctx, RE_FALSE));
	CAP("rsxSetBlendEnable", re_set_blend_enable(ctx, RE_FALSE));
	CAP("rsxSetLogicOpEnable", re_set_logic_op_enable(ctx, RE_FALSE));
	CAP("rsxSetCullFaceEnable", re_set_cull_face_enable(ctx, RE_FALSE));
	CAP("rsxSetDitherEnable", re_set_dither_enable(ctx, RE_FALSE));
	CAP("rsxSetZMinMaxControl", re_set_zminmax_control(ctx, RE_FALSE, RE_TRUE, RE_FALSE));
	CAP("rsxSetColorMask", re_set_color_mask(ctx, RE_COLOR_MASK_B | RE_COLOR_MASK_G | RE_COLOR_MASK_R | RE_COLOR_MASK_A));
	CAP("rsxSetColorMaskMrt", re_set_color_mask_mrt(ctx, 0));
}

/* texture unit 0: an 8-bit plane; its whole state loaded (and the cache invalidated) every time */
static void tex_set_unit(u8 unit, u8 location, u32 offset, u16 w, u16 h, u32 pitch);

static void tex_set(u8 location, u32 offset, u16 w, u16 h, u32 pitch)
{
	tex_set_unit(0, location, offset, w, h, pitch);
}

static void tex_set_unit(u8 unit, u8 location, u32 offset, u16 w, u16 h, u32 pitch)
{
	re_texture t;

	memset(&t, 0, sizeof(t));
	t.format = RE_TEX_B8 | RE_TEX_LIN | RE_TEX_UNRM;
	t.mipmap = 1;
	t.dimension = RE_TEX_2D;
	t.cubemap = RE_FALSE;
	t.remap = RE_TEX_REMAP_B_TO_ALL;
	t.width = w;
	t.height = h;
	t.depth = 1;
	t.location = location;
	t.pitch = pitch;
	t.offset = offset;
	CAP("rsxInvalidateTextureCache", re_invalidate_texture_cache(ctx, RE_INVALIDATE_TEXTURE));
	CAP("rsxLoadTexture", re_load_texture(ctx, unit, &t));
	CAP("rsxTextureControl", re_texture_control(ctx, unit, RE_TRUE, 0 << 8, 12 << 8, RE_TEX_ANISO_1));
	CAP("rsxTextureFilter", re_texture_filter(ctx, unit, 0, RE_TEX_NEAREST, RE_TEX_NEAREST, RE_TEX_QUINCUNX));
	CAP("rsxTextureWrapMode", re_texture_wrap_mode(ctx, unit, RE_TEX_CLAMP_TO_EDGE, RE_TEX_CLAMP_TO_EDGE, RE_TEX_CLAMP_TO_EDGE, 0,
	                   RE_TEX_ZFUNC_LESS, 0));
}

/* the fragment program for the draws that follow: W2, or W2A (the last pass of a compound block's second prediction) */
static void fp_load(int i)
{
	if (fp_cur == i)
		return;
	CAP("rsxLoadFragmentProgramLocation", re_load_fragment_program_location(ctx, fpx[i], fpx_off[i], RE_LOCATION_RSX));
	fp_cur = i;
}

static void fp_use(int avg)
{
	fp_load(fp_pick[shortp & 3][avg]);
}

/* W2A's constant: where the store surface's pixel lies for a target pixel of plane p in the slot's current row */
static void first_ofs(const mc_slot *s, int p)
{
	const f32 dy = (f32)RSXMC_STORE_DY(&geom, p, s->row), v[4] = { 0.0f, dy, 0.0f, dy };

	CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_first, v));
}

/* ---- command buffer and vertex ring ---------------------------------------------------------------------------- */

/* room for `need` command bytes: when the buffer's end comes near, the RSX reads everything so far and the buffer
   starts over */
static int cmd_room(int need)
{
	s64 t0;

	if (re_overflow) {                      /* (rsx_emit.h: a call that did not fit wrote nothing) */
		fail("a command found no room in the command buffer");
		return -1;
	}
	if (tv9h_cb_room() >= need + 256)
		return 0;
	t0 = now_us();
	rxp_flush(ctx);
	if (tv9h_cb_restart(WAIT_US) != 0) {
		fail("the RSX did not read the command buffer to its end");
		return -1;
	}
	st_restarts++;
	st_us_cb += (u64)(now_us() - t0);
	return 0;
}

/* `bytes` of the vertex ring that no draw under way reads; its start and the id the draw's label will carry. Whole
   cache lines (128 bytes): the writer clears them with dcbz first, which must not touch a neighbour's vertices */
static int ring_alloc(u32 bytes, u32 *start, u32 *id)
{
	s64 t0 = 0;

	bytes = (bytes + 127) & ~127u;
	for (;;) {
		const u32 done = *(vu32 *)rxp_label(LAB_CHUNK);
		const u32 s = ring_wr + bytes > ring_size ? 0 : ring_wr;
		int i, clash;

		rows_seen();
		while (fl_n && (s32)(done - fl[fl_head].id) >= 0) {
			fl_head = (fl_head + 1) & 255;
			fl_n--;
		}
		clash = fl_n >= 255;
		for (i = 0; i < fl_n && !clash; i++) {
			const int e = (fl_head + i) & 255;

			clash = fl[e].start < s + bytes && fl[e].end > s;
		}
		if (!clash) {
			const int e = (fl_head + fl_n) & 255;

			*start = s;
			*id = chunk_id + 1;             /* (the next label's value: written after some draws, see draw_list) */
			fl[e].start = s;
			fl[e].end = s + bytes;
			fl[e].id = chunk_id + 1;
			fl_n++;
			ring_wr = s + bytes;
			if (t0)
				st_us_ring += (u64)(now_us() - t0);
			return 0;
		}
		if (!t0) {                          /* the RSX draws what it has; then space comes back */
			t0 = now_us();
			st_ringwaits++;
			if (unlabeled) {
				CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
				unlabeled = 0;
			}
			rxp_flush(ctx);
		}
		if (now_us() - t0 > WAIT_US) {
			fail("the vertex ring did not come free");
			return -1;
		}
		tv9p_sleep_us(20);
	}
}

/* n rectangles drawn: their vertices into the ring, a draw through the index buffer, the draw's label */
static int draw_list(const rsxmc_rect *r, u32 n)
{
	while (n) {
		const u32 c = n < CHUNK_RECTS ? n : CHUNK_RECTS;
		u32 start, id, i;
		vtx *v;

		(void)sizeof(char[sizeof(vtx) == 48 ? 1 : -1]);   /* four vertices are 24 doublewords (the SDK's GCC 4.1 has no _Static_assert) */
		const u32 vsz = compact ? 16 : (u32)sizeof(vtx);

		if (ring_alloc(c * 4 * vsz, &start, &id) != 0)
			return -1;
		v = (vtx *)(ring + start);
		/* the lines cleared in the cache, not fetched from memory first (a store into a line not cached costs the PPU
		   a memory read of the whole line) */
		for (i = 0; i < c * 4 * vsz; i += 128)
			__asm__ volatile("dcbz 0,%0" : : "r"(ring + start + i) : "memory");
		if (compact) {                      /* 8 doublewords a rectangle: (x, y, u, v) as 16 bits each, then the taps' bytes */
			u64 *w = (u64 *)v;

			for (i = 0; i < c; i++, r++, w += 8) {
				const u64 x0 = (u16)r->x0, x1 = (u16)r->x1, y0 = (u16)r->y0, y1 = (u16)r->y1;
				const u64 u0 = (u16)(r->x0 + r->dx), u1 = (u16)(r->x1 + r->dx), v0 = (u16)(r->y0 + r->dy), v1 = (u16)(r->y1 + r->dy);
				const u64 k = ktab8[(r->set & 3) * 16 + (r->pos & 15)];

				w[0] = x0 << 48 | y0 << 32 | u0 << 16 | v0;
				w[1] = k;
				w[2] = x1 << 48 | y0 << 32 | u1 << 16 | v0;
				w[3] = k;
				w[quads ? 6 : 4] = x0 << 48 | y1 << 32 | u0 << 16 | v1;   /* (a quad's corners go around it) */
				w[5] = k;
				w[quads ? 4 : 6] = x1 << 48 | y1 << 32 | u1 << 16 | v1;
				w[7] = k;
			}
		} else {
			/* 24 doubleword stores a rectangle, everything else in registers (a value stored and read back at once costs
			   the PPU a load-hit-store stall of some 40 cycles) */
			u64 *w = (u64 *)v;

			for (i = 0; i < c; i++, r++, w += 24) {
				const u64 x0 = fbits(r->x0), x1 = fbits(r->x1), y0 = fbits(r->y0), y1 = fbits(r->y1);
				const u64 u0 = fbits(r->x0 + r->dx), u1 = fbits(r->x1 + r->dx), v0 = fbits(r->y0 + r->dy), v1 = fbits(r->y1 + r->dy);
				const u64 *k = ktab[(r->set & 3) * 16 + (r->pos & 15)];
				const u64 k0 = k[0], k1 = k[1], k2 = k[2], k3 = k[3];

				w[0] = x0 << 32 | y0;
				w[1] = u0 << 32 | v0;
				w[2] = k0;
				w[3] = k1;
				w[4] = k2;
				w[5] = k3;
				w[6] = x1 << 32 | y0;
				w[7] = u1 << 32 | v0;
				w[8] = k0;
				w[9] = k1;
				w[10] = k2;
				w[11] = k3;
				w[12] = x0 << 32 | y1;
				w[13] = u0 << 32 | v1;
				w[14] = k0;
				w[15] = k1;
				w[16] = k2;
				w[17] = k3;
				w[18] = x1 << 32 | y1;
				w[19] = u1 << 32 | v1;
				w[20] = k0;
				w[21] = k1;
				w[22] = k2;
				w[23] = k3;
			}
		}
		if (cmd_room(DRAW_CMD) != 0)
			return -1;
		re_invalidate_vertex_cache(ctx);
		if (compact) {                      /* vertex types 5 (signed 16-bit integers) and 7 (bytes 0..255), as they are */
			CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_POS, 0, ring_off + start, 16, 2, 5, RE_LOCATION_CELL));
			CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX0, 0, ring_off + start + 4, 16, 2, 5, RE_LOCATION_CELL));
			CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX1, 0, ring_off + start + 8, 16, 4, 7, RE_LOCATION_CELL));
			CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX2, 0, ring_off + start + 12, 16, 4, 7, RE_LOCATION_CELL));
		} else {
		CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_POS, 0, ring_off + start, sizeof(vtx), 2, RE_VERTEX_F32,
		                         RE_LOCATION_CELL));
		CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX0, 0, ring_off + start + 8, sizeof(vtx), 2, RE_VERTEX_F32,
		                         RE_LOCATION_CELL));
		CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX1, 0, ring_off + start + 16, sizeof(vtx), 4, RE_VERTEX_F32,
		                         RE_LOCATION_CELL));
		CAP("rsxBindVertexArrayAttrib", re_bind_vertex_array_attrib(ctx, RE_ATTRIB_TEX2, 0, ring_off + start + 32, sizeof(vtx), 4, RE_VERTEX_F32,
		                         RE_LOCATION_CELL));
		}
		if (quads && compact)
			CAP("rsxDrawVertexArray", re_draw_vertex_array(ctx, RE_TYPE_QUADS, 0, 4 * c));
		else
			CAP("rsxDrawIndexArray", re_draw_index_array(ctx, RE_TYPE_TRIANGLES, idx_off, 6 * c, RE_INDEX_16B, RE_LOCATION_RSX));
		(void)id;
		/* a back-end label costs the RSX 3 us (probe 5.1): one for every 256 KB of vertices and one a row */
		unlabeled += c * 4 * vsz;
		if (unlabeled >= LABEL_EVERY) {
			CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
			unlabeled = 0;
		}
		st_draws++;
		st_vtx_bytes += c * 4 * vsz;
		n -= c;
	}
	return 0;
}

/* ---- a row ------------------------------------------------------------------------------------------------------- */

/* the reference rows the slot's next row reads are final */
static int released(const mc_slot *s, const row_meta *m)
{
	int k;

	for (k = 0; k < 3; k++) {
		const volatile pix_prog *p = s->d.prog[k];

		if (p && (m->need_y[k] > p->lim_y || m->need_c[k] > p->lim_c))
			return 0;
	}
	return 1;
}

/* reference k's plane p as the texture */
static void ref_tex(const mc_slot *s, int k, int p)
{
	const u32 st = p ? s->d.stride_c : s->d.stride_y;

	tex_set(RE_LOCATION_CELL, s->tex[k][p], (u16)st,
	        (u16)(p ? geom.ah / 2 + 2 * geom.border_c : geom.ah + 2 * geom.border_y), st);
}

/* slot's marker: 16 bytes v, v + 1, ... drawn into the marker surface (the target switches around it: what was drawn
   before leaves for memory before the marker, the marker before anything later) */
static int marker_draw(int slot, int v)
{
	const rsxmc_rect mr = { 0, (int16_t)slot, 16, (int16_t)(slot + 1), (int16_t)(16 + v), (int16_t)-slot, 0, 0, { 0, 0 } };

	if (cmd_room(4096) != 0)
		return -1;
	target_set(RE_LOCATION_CELL, mark_off, 64, 64, 64);
	taps(0);
	tex_set(RE_LOCATION_RSX, ramp_off, 512, 8, 512);
	if (draw_list(&mr, 1) != 0)
		return -1;
	target_set(RE_LOCATION_RSX, mid_off, (u32)geom.aw, (u16)geom.aw, RSXMC_MID_H);
	return 0;
}

/* the surface in main memory a row's predictions end in: the slot's frame-shaped one, or (packed) the row's own */
static void main_target(const mc_slot *s, int slot)
{
	const u32 off = pred_off0 + (u32)slot * (u32)surf_bytes;

	if (geom.strips)
		target_set(RE_LOCATION_CELL, off, RSXMC_STRIP_PITCH, RSXMC_STRIP_PITCH, (u16)RSXMC_STRIP_ROWS(&geom));
	else if (geom.packed)
		target_set(RE_LOCATION_CELL, off + (u32)s->row * (u32)RSXMC_ROW_BYTES(&geom), 64, 64, (u16)RSXMC_ROW_H(&geom));
	else
		target_set(RE_LOCATION_CELL, off, (u32)geom.aw, (u16)geom.aw, (u16)(geom.ah + geom.ah / 2));
}

/* the render target a row's predictions are drawn to: that surface, or the packed one's stand-in in RSX memory */
static void pred_target(const mc_slot *s, int slot)
{
	if (geom.packed && ploc_use && !geom.strips)
		target_set(RE_LOCATION_RSX, ploc_off, 64, 64, (u16)RSXMC_ROW_H(&geom));
	else
		main_target(s, slot);
}

static int emit_row(mc_slot *s, int slot, const row_lists *L)
{
	const int aw = geom.aw;
	u8 *c0;
	int p, k, any_mid = 0, any_amid = 0, any_a = 0, any_bmid = 0, any_b = 0, rc = -1;

	const u32 restarts0 = st_restarts, ringwaits0 = st_ringwaits;

	tv9h_lock();
	if (cmd_room(ROW_CMD) != 0)
		goto out;
	if (state_seen != tv9h_state_gen())      /* (3.70) the display drew a frame since: its programs are loaded, not ours */
		programs_load();
	c0 = (u8 *)ctx->current;
	{                                       /* the RSX's backlog: vertex bytes of draws not done yet */
		const u32 done = *(vu32 *)rxp_label(LAB_CHUNK);
		u64 fly = 0;
		int i;

		for (i = 0; i < fl_n; i++) {
			const int e = (fl_head + i) & 255;

			if ((s32)(done - fl[e].id) < 0)
				fly += fl[e].end - fl[e].start;
		}
		st_fly_bytes += fly;
		st_fly_n++;
		if (fly > st_fly_max)
			st_fly_max = fly;
	}
	for (p = 0; p < 3; p++)
		for (k = 0; k < 3; k++) {
			any_mid |= L->n[RSXMC_L_MID(p, k)] != 0;
			any_amid |= L->n[RSXMC_L_AMID(p, k)] != 0;
			any_a |= L->n[RSXMC_L_AMID(p, k)] || L->n[RSXMC_L_AH(p, k)] || L->n[RSXMC_L_AV(p, k)];
			any_bmid |= L->n[RSXMC_L_BMID(p, k)] != 0;
			any_b |= L->n[RSXMC_L_BMID(p, k)] || L->n[RSXMC_L_BH(p, k)] || L->n[RSXMC_L_BV(p, k)];
		}
	if (L->rects) {
#define DRAW(list)                                                                                          \
	do {                                                                                                    \
		if (L->n[list] && draw_list(L->r[list], L->n[list]) != 0)                                           \
			goto out;                                                                                       \
	} while (0)
#define DRAW_REF(list)                                                                                      \
	do {                                                                                                    \
		if (L->n[list]) {                                                                                   \
			ref_tex(s, k, p);                                                                               \
			DRAW(list);                                                                                     \
		}                                                                                                   \
	} while (0)
/* written before they are read: the RSX waits for its own back end */
#define SYNC()                                                                                              \
	do {                                                                                                    \
		if (sync_on) {                                                                                      \
			sync_id++;                                                                                      \
			CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_SYNC, sync_id));                                                \
			CAP("rsxSetWaitLabel", re_set_wait_label(ctx, LAB_SYNC, sync_id));                                                        \
		}                                                                                                   \
	} while (0)
		fp_use(0);
		taps(0);
		if (any_mid || any_amid) {          /* the two-pass blocks' horizontal passes (compound: the first predictions') */
			target_set(RE_LOCATION_RSX, mid_off, (u32)aw, (u16)aw, RSXMC_MID_H);
			for (p = 0; p < 3; p++)
				for (k = 0; k < 3; k++) {
					DRAW_REF(RSXMC_L_MID(p, k));
					DRAW_REF(RSXMC_L_AMID(p, k));
				}
			SYNC();
		}
		if (any_a) {                        /* the compound blocks' first predictions into the store surface */
			if (geom.packed)                /* (the same bytes, in the shape of a row's prediction surface) */
				target_set(RE_LOCATION_RSX, store_off, 64, 64, (u16)RSXMC_ROW_H(&geom));
			else
				target_set(RE_LOCATION_RSX, store_off, (u32)aw, (u16)aw, RSXMC_STORE_H);
			for (p = 0; p < 3; p++)
				for (k = 0; k < 3; k++)
					DRAW_REF(RSXMC_L_AH(p, k));
			taps(1);
			for (p = 0; p < 3; p++)
				for (k = 0; k < 3; k++)
					DRAW_REF(RSXMC_L_AV(p, k));
			if (L->n[RSXMC_L_AVMID]) {
				tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
				DRAW(RSXMC_L_AVMID);
			}
			taps(0);
		}
		pred_target(s, slot);
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++)
				DRAW_REF(RSXMC_L_H(p, k));
		taps(1);
		for (p = 0; p < 3; p++)
			for (k = 0; k < 3; k++)
				DRAW_REF(RSXMC_L_V(p, k));
		if (L->n[RSXMC_L_VMID]) {
			tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
			DRAW(RSXMC_L_VMID);
		}
		if (any_b || L->n[RSXMC_L_BVMID(0)] || L->n[RSXMC_L_BVMID(1)]) {
			/* the second predictions: their horizontal passes over the first ones' in the middle surface (those are
			   read by now), then the last passes, which average with the store surface */
			if (any_bmid) {
				target_set(RE_LOCATION_RSX, mid_off, (u32)aw, (u16)aw, RSXMC_MID_H);
				taps(0);
				for (p = 0; p < 3; p++)
					for (k = 0; k < 3; k++)
						DRAW_REF(RSXMC_L_BMID(p, k));
				pred_target(s, slot);
			}
			SYNC();                         /* (the store surface and the middle one) */
			fp_use(1);
			if (geom.packed)
				tex_set_unit(1, RE_LOCATION_RSX, store_off, 64, (u16)RSXMC_ROW_H(&geom), 64);
			else
				tex_set_unit(1, RE_LOCATION_RSX, store_off, (u16)aw, RSXMC_STORE_H, (u32)aw);
			for (p = 0; p < 3; p++) {
				first_ofs(s, p);
				taps(0);
				for (k = 0; k < 3; k++)
					DRAW_REF(RSXMC_L_BH(p, k));
				taps(1);
				for (k = 0; k < 3; k++)
					DRAW_REF(RSXMC_L_BV(p, k));
			}
			for (p = 0; p < 2; p++)
				if (L->n[RSXMC_L_BVMID(p)]) {
					first_ofs(s, p);
					tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
					DRAW(RSXMC_L_BVMID(p));
				}
			fp_use(0);
			st_two_rows++;
		}
		if (geom.packed && ploc_use && !geom.strips) {   /* the row out of RSX memory into main memory: one rectangle */
			static const f32 zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const rsxmc_rect cr = { 0, 0, 64, (int16_t)RSXMC_ROW_H(&geom), 0, 0, 0, 0, { 0, 0 } };

			SYNC();
			main_target(s, slot);
			fp_load(FP_COPY);
			CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_tap[0], zero));   /* (texcoord 0 = the pixel itself) */
			tex_set(RE_LOCATION_RSX, ploc_off, 64, (u16)RSXMC_ROW_H(&geom), 64);
			if (draw_list(&cr, 1) != 0)
				goto out;
			fp_use(0);
			st_copies++;
		}
#undef DRAW
#undef DRAW_REF
#undef SYNC
	}
	if (cmd_room(2048) != 0)
		goto out;
	if (unlabeled) {
		CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
		unlabeled = 0;
	}
	/* The back-end label does not say that every pixel is in main memory: single pixel rows of a row's last rectangles
	   were seen to arrive some 10 us after it (3.41 / 3.42: a pixel SPU that had waited for the label and read at once got
	   the surface's old bytes there). A switch of the render target before the label does not change that (3.42d) */
	if (row_flush && L->rects)
		target_set(RE_LOCATION_RSX, mid_off, (u32)aw, (u16)aw, RSXMC_MID_H);
	/* ... so the pixel SPUs wait for a marker instead that takes the pixels' own way into main memory: 16 bytes drawn
	   after the row's rectangles (the target switches around it: the row's pixels leave before the marker, the marker
	   before anything later) */
	if (row_marker) {
		if (marker_draw(slot, (slot_gen[slot] & 7) << 5 | (s->row + 1)) != 0 || cmd_room(64) != 0)
			goto out;
		if (unlabeled) {
			CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
			unlabeled = 0;
		}
	}
	CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, (u8)(LAB_ROW0 + slot), s->base + (u32)s->row));
	if ((u8 *)ctx->current > c0)
		st_cmd_bytes += (u64)((u8 *)ctx->current - c0);
	if (!bench_noflush)
		rxp_flush(ctx);
	s->t_emit[s->row & 63] = now_us();
	s->rowflag[s->row & 63] = (u8)((st_restarts != restarts0 ? 1 : 0) | (st_ringwaits != ringwaits0 ? 2 : 0) | (any_mid ? 4 : 0));
	rc = 0;
out:
	tv9h_unlock();
	return rc;
}

/* ---- tests of the failure paths (rsx_mc_test_inject) ------------------------------------------------------------ */

static volatile int test_fail_rows, test_stall_rows, test_stall_ms;
static volatile s64 test_stall_t;           /* when the RSX was stopped (0: it is not) */
static u32 test_rows;                       /* rows drawn since start */

/* the RSX's front end waits at a label until test_stall_end */
static void test_stall_begin(void)
{
	tv9h_lock();
	*(vu32 *)rxp_label(LAB_TEST) = 0;
	__asm__ volatile("sync" ::: "memory");
	if (cmd_room(64) == 0) {
		re_set_wait_label(ctx, LAB_TEST, 1);
		rxp_flush(ctx);
		test_stall_t = now_us();
		log_line("rsx_mc: TEST: the RSX stands still from row %u on, for %d ms", (unsigned)test_rows, test_stall_ms);
	}
	tv9h_unlock();
}

/* (any thread, often) the RSX goes on when its time is up - or at once (now) */
static void test_stall_end(int now)
{
	const s64 t = test_stall_t;

	if (t && (now || now_us() - t > (s64)test_stall_ms * 1000)) {
		*(vu32 *)rxp_label(LAB_TEST) = 1;
		__asm__ volatile("sync" ::: "memory");
		test_stall_t = 0;
		log_line("rsx_mc: TEST: the RSX goes on after %.0f ms", (double)(now_us() - t) / 1e3);
	}
}

void rsx_mc_test_inject(int fail_rows, int stall_rows, int stall_ms)
{
	test_fail_rows = fail_rows;
	test_stall_rows = stall_rows;
	test_stall_ms = stall_ms;
}

/* ---- the worker threads: one builds the rows' rectangles ahead, one draws the rows as their references allow ---- */

static void slot_order(int *order, int *n)
{
	int i, j;

	*n = 0;
	for (i = 0; i < RSX_MC_SLOTS; i++)
		if (slots[i].state == 1) {          /* the earlier frame first */
			for (j = (*n)++; j > 0 && slots[order[j - 1]].d.order > slots[i].d.order; j--)
				order[j] = order[j - 1];
			order[j] = i;
		}
}

/* room for `bytes` in the slot's ring of built rows: where, or -1 while the rows not drawn yet are in the way */
static int rows_room(mc_slot *s, u32 bytes)
{
	const u32 at = s->r_wr + bytes > ROWS_BYTES ? 0 : s->r_wr;
	int r;

	for (r = s->row; r < s->b_row; r++)
		if (s->rows[r].start < at + bytes && s->rows[r].start + s->rows[r].bytes > at)
			return -1;
	return (int)at;
}

static void builder(void *arg)
{
	(void)arg;
	while (!stop_flag) {
		int order[RSX_MC_SLOTS], n, j, progress = 0;

		test_stall_end(0);

		slot_order(order, &n);
		for (j = 0; j < n; j++) {
			mc_slot *s = &slots[order[j]];

			s->building = 1;
			__asm__ volatile("sync" ::: "memory");
			while (s->state == 1 && !s->abort && !failed && !stop_flag && s->b_row < s->d.sb_rows) {
				row_meta *m = &s->rows[s->b_row];
				int at, l;
				u32 o = 0;

				if (!s->built) {
					const s64 t0 = now_us();

					rsxmc_reset(&s->L);
					s->d.fn(s->d.user, s->b_row, &s->L);
					st_us_build += (u64)(now_us() - t0);
					if (s->L.failed || s->L.rects * sizeof(rsxmc_rect) > ROWS_BYTES) {
						fail("a row's rectangles could not be built");
						break;
					}
					s->built = 1;
				}
				at = rows_room(s, s->L.rects * (u32)sizeof(rsxmc_rect));
				if (at < 0)
					break;                   /* (the drawing thread makes room) */
				for (l = 0; l < RSXMC_NLIST; l++) {
					memcpy(s->ring + at + o, s->L.r[l], s->L.n[l] * sizeof(rsxmc_rect));
					m->n[l] = s->L.n[l];
					o += s->L.n[l] * (u32)sizeof(rsxmc_rect);
				}
				m->start = (u32)at;
				m->bytes = o;
				m->rects = s->L.rects;
				for (l = 0; l < 3; l++) {
					m->need_y[l] = s->L.need_y[l];
					m->need_c[l] = s->L.need_c[l];
				}
				s->r_wr = (u32)at + o;
				s->built = 0;
				__asm__ volatile("sync" ::: "memory");
				s->b_row++;
				progress = 1;
			}
			s->building = 0;
		}
		if (!progress)
			tv9p_sleep_us(50);
	}
}

static void worker(void *arg)
{
	(void)arg;
	while (!stop_flag) {
		int order[RSX_MC_SLOTS], n, j, progress = 0;

		slot_order(order, &n);
		for (j = 0; j < n; j++) {
			mc_slot *s = &slots[order[j]];

			while (s->state == 1 && !stop_flag) {
				const row_meta *m = &s->rows[s->row];
				row_lists L;
				s64 t0;
				u32 o;
				int l;

				if (s->abort || failed) {
					s->state = 2;
					break;
				}
				if (s->row >= s->b_row || !released(s, m))
					break;
				for (l = 0, o = 0; l < RSXMC_NLIST; l++) {
					L.r[l] = (const rsxmc_rect *)(s->ring + m->start + o);
					o += m->n[l] * (u32)sizeof(rsxmc_rect);
				}
				L.n = m->n;
				L.rects = m->rects;
				t0 = now_us();
				if (emit_row(s, order[j], &L) != 0)
					break;
				st_us_emit += (u64)(now_us() - t0);
				st_rows++;
				test_rows++;
				if (test_fail_rows && test_rows == (u32)test_fail_rows)
					fail("TEST: a failure after this many rows was asked for");
				if (test_stall_rows && test_rows == (u32)test_stall_rows)
					test_stall_begin();
				st_rects += m->rects;
				if (m->rects > st_row_max)
					st_row_max = m->rects;
				progress = 1;
				if (s->row + 1 == s->d.sb_rows) {
					st_frames++;
					s->row++;
					__asm__ volatile("sync" ::: "memory");
					s->state = 2;
				} else {
					__asm__ volatile("sync" ::: "memory");
					s->row++;
				}
			}
		}
		rows_seen();
		if (!progress) {
			st_idle++;
			tv9p_sleep_us(50);
		}
	}
	running = 0;
}

/* ---- set-up ------------------------------------------------------------------------------------------------------- */

int rsx_mc_prealloc(int aw, int ah)
{
	if (pred_mem)
		return 0;
	{                                       /* (a slot's surface: frame-shaped, or cells of 192 x 32 - whichever is larger) */
		const rsxmc_geom g = { aw, ah, 0, 0, 0, 0, 0, 0, 0 };
		const size_t fs = (size_t)aw * (size_t)ah * 3 / 2, ss = RSXMC_STRIP_BYTES(&g);

		pred_bytes = ((size_t)RSX_MC_SLOTS * (fs > ss ? fs : ss) + MARK_BYTES + 0xFFFFF) & ~(size_t)0xFFFFF;
	}
	pred_mem = tv9p_alloc(pred_bytes, 1024 * 1024);
	return pred_mem ? 0 : -1;
}

void rsx_mc_compact(int on)
{
	compact_want = on;
}

void rsx_mc_row_flush(int on)
{
	row_flush = on;
}

void rsx_mc_sizes(uint32_t mask)
{
	geom.sizes = mask;
}

void rsx_mc_compound(int on)
{
	two_want = on;
}

void rsx_mc_short(int on)
{
	short_want = on;
}

void rsx_mc_quads(int on)
{
	quads = on;
}

void rsx_mc_packed(int on)
{
	packed_want = on;
}

void rsx_mc_sync(int on)
{
	sync_on = on;
}

void rsx_mc_strips(int on)
{
	strips_want = on;
}

void rsx_mc_row_marker(int on)
{
	row_marker = on;
}

void rsx_mc_marker(int slot, uint64_t *ea, uint32_t *gen)
{
	*ea = (uint64_t)(uintptr_t)(pred_mem + pred_bytes - MARK_BYTES + (size_t)slot * 64);
	*gen = slot_gen[slot] & 7u;
}

const rsxmc_geom *rsx_mc_geom(void)
{
	return &geom;
}

/* the vertex program for the vertex format asked for (rsx_mc_compact), and its constants */
static int vp_select(void)
{
	u32 size;

	compact = compact_want;
	two = two_want && !strips_want;         /* (the cell layout has no blocks with two references) */
	geom.compound = two;
	vp = (const re_vp *)(const void *)(two ? (compact ? tv9_prog_vp_w2ca : tv9_prog_vp_w2a) : compact ? tv9_prog_vp_w2c : tv9_prog_vp_w2);
	vp_ucode = re_vp_ucode(vp, &size);
	vc_first = two ? re_vp_const(vp, "firstOfs") : NULL;
	if (two && !vc_first) {
		log_line("rsx_mc: the vertex program for compound blocks lacks its constant");
		return -1;
	}
	vc_bias = compact ? re_vp_const(vp, "tapBias") : NULL;
	if (compact && !vc_bias) {
		log_line("rsx_mc: the compact vertex program lacks its constant");
		return -1;
	}
	vc_scale = re_vp_const(vp, "posScale");
	vc_offset = re_vp_const(vp, "posOffset");
	vc_tap[0] = re_vp_const(vp, "tapOfs01");
	vc_tap[1] = re_vp_const(vp, "tapOfs23");
	vc_tap[2] = re_vp_const(vp, "tapOfs45");
	vc_tap[3] = re_vp_const(vp, "tapOfs67");
	if (!vc_scale || !vc_offset || !vc_tap[0] || !vc_tap[1] || !vc_tap[2] || !vc_tap[3]) {
		log_line("rsx_mc: the vertex program lacks a constant");
		return -1;
	}
	return 0;
}

int rsx_mc_init(void *const *fb, int n, size_t fb_bytes, int aw, int ah)
{
	u32 off, size;
	const void *uc;
	int k;

	rsx_mc_release();
	ctx = tv9h_context();
	{
		const rsxmc_geom g = { aw, ah, 0, 0, 0, 0, 0, 0, 0 };

		surf_bytes = (size_t)aw * (size_t)ah * 3 / 2;
		if (RSXMC_STRIP_BYTES(&g) > surf_bytes)
			surf_bytes = RSXMC_STRIP_BYTES(&g);
	}
	if (!ctx || n > MAX_FB || (fb_bytes & 0xFFFFF) || !pred_mem || surf_bytes * RSX_MC_SLOTS + MARK_BYTES > pred_bytes ||
	    ah + ah / 2 > 4096 || aw > 4096) {
		log_line("rsx_mc: no context, no prediction memory, or frames of %d x %d are too big", aw, ah);
		return -1;
	}
	for (k = 0; k < n; k++) {
		if (((uintptr_t)fb[k] & 0xFFFFF) || rxp_map_main(fb[k], (u32)fb_bytes, &off) != 0) {
			log_line("rsx_mc: frame buffer region %d (%p, %u bytes) could not be mapped for the RSX", k, fb[k], (unsigned)fb_bytes);
			rsx_mc_release();
			return -1;
		}
		fb_mapped[fb_n++] = fb[k];
	}
	if (rxp_map_main(pred_mem, (u32)pred_bytes, &pred_off0) != 0) {
		log_line("rsx_mc: the prediction surfaces (%u bytes) could not be mapped for the RSX", (unsigned)pred_bytes);
		rsx_mc_release();
		return -1;
	}
	pred_mapped = 1;
	memset(pred_mem, 0, pred_bytes);
	cache_flush(pred_mem, pred_bytes);
	geom.aw = aw;
	geom.ah = ah;
	geom.packed = packed_want != 0;
	geom.strips = strips_want ? RSXMC_STRIP_H : 0;
	ploc_use = packed_want == 2;
	geom.border_y = PIX_BORDER_Y;
	geom.border_c = PIX_BORDER_C;
	ploc = tv9h_local_alloc(128, (u32)aw * RSXMC_STORE_H);
	mid = tv9h_local_alloc(128, (u32)aw * RSXMC_MID_H);
	idx = tv9h_local_alloc(128, 6 * CHUNK_RECTS * sizeof(u16));
	ramp = tv9h_local_alloc(128, 512 * 8);
	if (ramp) {
		for (k = 0; k < 512 * 8; k++)
			ramp[k] = (uint8_t)((k & 511) - 16);
		ramp_off = offset_of(ramp);
	}
	mark_off = pred_off0 + (u32)(pred_bytes - MARK_BYTES);
	store = tv9h_local_alloc(128, (u32)aw * RSXMC_STORE_H);
	if (ploc)
		ploc_off = offset_of(ploc);
	if (!mid || !idx || !ramp || !store || !ploc) {
		log_line("rsx_mc: no RSX memory for the middle surface, the store surface or the indices");
		rsx_mc_release();
		return -1;
	}
	store_off = offset_of(store);
	fpx[0] = (const re_fp *)(const void *)tv9_prog_w2;
	fpx[1] = (const re_fp *)(const void *)tv9_prog_w2a;
	fpx[2] = (const re_fp *)(const void *)tv9_prog_w3;
	fpx[3] = (const re_fp *)(const void *)tv9_prog_w3a;
	fpx[4] = (const re_fp *)(const void *)tv9_prog_w3t;
	fpx[5] = (const re_fp *)(const void *)tv9_prog_w3ta;
	fpx[6] = (const re_fp *)(const void *)tv9_prog_w3k;
	fpx[FP_COPY] = (const re_fp *)(const void *)tv9_prog_cp;
	for (k = 0; k < NFP; k++) {
		uc = re_fp_ucode(fpx[k], &size);
		fp_mem[k] = tv9h_local_alloc(64, size);
		if (!fp_mem[k]) {
			log_line("rsx_mc: no RSX memory for a fragment program");
			rsx_mc_release();
			return -1;
		}
		memcpy(fp_mem[k], uc, size);
		fpx_off[k] = offset_of(fp_mem[k]);
	}
	mid_off = offset_of(mid);
	idx_off = offset_of(idx);
	for (k = 0; k < CHUNK_RECTS; k++) {     /* two triangles a rectangle: (x0 y0, x1 y0, x0 y1), (x1 y0, x1 y1, x0 y1) */
		static const u8 q[6] = { 0, 1, 2, 1, 3, 2 };
		int j;

		for (j = 0; j < 6; j++)
			idx[6 * k + j] = (u16)(4 * k + q[j]);
	}
	if (vp_select() != 0) {
		rsx_mc_release();
		return -1;
	}
	for (k = 0; k < FT_N; k++) {
		union {
			f32 f;
			u32 u;
		} c;

		c.f = (f32)(k - FT_OFF);
		ftab[k] = c.u;
	}
	for (k = 0; k < 64; k++) {
		const int16_t *t = rsxmc_kernel(k >> 4, k & 15);
		int j;

		for (j = 0; j < 4; j++)
			ktab[k][j] = (u64)fbits(t[2 * j]) << 32 | fbits(t[2 * j + 1]);
		ktab8[k] = 0;
		for (j = 0; j < 8; j++)
			ktab8[k] = ktab8[k] << 8 | (u64)(u8)(t[j] + 64);
	}
	/* the vertex ring: main memory of the block's own, mapped for the RSX (until 3.70 the test application's, behind
	   its command buffer) */
	if (!ring)
		ring = tv9p_alloc(RING_BYTES, 1024 * 1024);
	if (!ring || rxp_map_main(ring, RING_BYTES, &ring_off) != 0) {
		log_line("rsx_mc: no memory for the vertex ring, or it could not be mapped for the RSX");
		rsx_mc_release();
		return -1;
	}
	ring_mapped = 1;
	ring_size = RING_BYTES;
	ring_wr = 0;
	fl_head = fl_n = 0;
	*(vu32 *)rxp_label(LAB_CHUNK) = chunk_id;
	memset(slots, 0, sizeof(slots));
	for (k = 0; k < RSX_MC_SLOTS; k++) {
		slots[k].ring = malloc(ROWS_BYTES);
		if (!slots[k].ring) {
			log_line("rsx_mc: no memory for the rows' rectangles");
			rsx_mc_release();
			return -1;
		}
	}
	log_line("rsx_mc: vertices of %d bytes (%s)", compact ? 16 : 48, compact ? "16-bit coordinates, 8-bit taps" : "floats");
	log_line("rsx_mc: ready for %d x %d: %d frame buffer region(s) of %u MB and %d prediction surfaces (%u MB) mapped; middle "
	         "surface %d x %d and indices in RSX memory; vertex ring %u KB of main memory (%d bytes of commands free)", aw, ah, n,
	         (unsigned)(fb_bytes >> 20), RSX_MC_SLOTS, (unsigned)(pred_bytes >> 20), aw, RSXMC_MID_H, (unsigned)(ring_size >> 10),
	         tv9h_cb_room());
	ready = 1;
	return 0;
}

void rsx_mc_release(void)
{
	int k;

	if (running)
		rsx_mc_stop();
	ready = 0;
	for (k = 0; k < fb_n; k++)
		rxp_unmap_main(fb_mapped[k]);
	fb_n = 0;
	if (pred_mem && pred_mapped)
		rxp_unmap_main(pred_mem);      /* (the memory itself stays for the next stream) */
	pred_mapped = 0;
	if (ring && ring_mapped)
		rxp_unmap_main(ring);
	ring_mapped = 0;
	if (mid)
		tv9h_local_free(mid);
	mid = NULL;
	if (idx)
		tv9h_local_free(idx);
	idx = NULL;
	if (store)
		tv9h_local_free(store);
	store = NULL;
	if (ploc)
		tv9h_local_free(ploc);
	ploc = NULL;
	if (ramp)
		tv9h_local_free(ramp);
	ramp = NULL;
	for (k = 0; k < NFP; k++) {
		if (fp_mem[k])
			tv9h_local_free(fp_mem[k]);
		fp_mem[k] = NULL;
	}
	for (k = 0; k < RSX_MC_SLOTS; k++) {
		rsxmc_free(&slots[k].L);
		free(slots[k].ring);
		slots[k].ring = NULL;
	}
}

#ifdef RSX_MC_BENCH
/* ---- what the RSX's steps cost (the probe's measurement; nothing of it runs in the decoder) ----------------------- */

static u32 bench_id = 0x51000000u;

/* everything written since the last flush handed to the RSX at once; the time until its last label is seen, in us
   (-1: it did not come) */
static double bench_run(void)
{
	const u32 id = ++bench_id;
	s64 t0;

	CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
	unlabeled = 0;
	CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_ROW0, id));
	t0 = now_us();
	rxp_flush(ctx);
	while (*(vu32 *)rxp_label(LAB_ROW0) != id)
		if (now_us() - t0 > WAIT_US) {
			fail("bench: the label did not come");
			return -1.0;
		}
	return (double)(now_us() - t0);
}

/* the region rw x rh at (0, 0) as rectangles w x h, a row of them every `pitch_y` rows; source: the same place of the
   texture + (dx, dy) */
static u32 bench_tile(rsxmc_rect *r, int rw, int rh, int w, int h, int pitch_y, int dx, int dy, int set, int pos)
{
	u32 n = 0;
	int x, y;

	for (y = 0; y + h <= rh; y += pitch_y)
		for (x = 0; x + w <= rw; x += w, n++) {
			r[n].x0 = (int16_t)x;
			r[n].y0 = (int16_t)y;
			r[n].x1 = (int16_t)(x + w);
			r[n].y1 = (int16_t)(y + h);
			r[n].dx = (int16_t)dx;
			r[n].dy = (int16_t)dy;
			r[n].set = (uint8_t)set;
			r[n].pos = (uint8_t)pos;
			r[n].pad[0] = r[n].pad[1] = 0;
		}
	return n;
}

void rsx_mc_bench(void)
{
	enum { N = 100, MAXR = 58880 };
	const int aw = geom.aw, by = geom.border_y;
	const u32 ref_off = fb_n ? offset_of(fb_mapped[0]) : 0, pred0 = pred_off0, pred1 = pred_off0 + (u32)surf_bytes;
	const u16 ref_w = (u16)(aw + 2 * by), ref_h = (u16)(geom.ah + 2 * by), surf_h = (u16)(geom.ah + geom.ah / 2);
	rsxmc_rect *r = malloc(2 * MAXR * sizeof(*r)), *r2 = r ? r + MAXR : NULL;
	rsxmc_rect one;
	double t, best, base = 0;
	int rep, i, test;

	if (!ready || running || !r || !fb_n || !two) {
		log_line("bench: not ready (it wants the module set up for compound blocks, and not running)");
		free(r);
		return;
	}
	bench_tile(&one, 8, 8, 8, 8, 8, by, by, 0, 5);
	tv9h_predicting(1);
	tv9h_lock();
	if (cmd_room(8192) != 0)
		goto out;
	programs_load();
	tex_set_unit(1, RE_LOCATION_CELL, pred1, (u16)aw, surf_h, (u32)aw);
	{
		const f32 z[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_first, z));
	}
	if (bench_run() < 0)
		goto out;
#define REF_TEX() tex_set(RE_LOCATION_CELL, ref_off, ref_w, ref_h, (u32)ref_w)
#define PRED_TGT() target_set(RE_LOCATION_CELL, pred0, (u32)aw, (u16)aw, surf_h)
#define MID_TGT() target_set(RE_LOCATION_RSX, mid_off, (u32)aw, (u16)aw, RSXMC_MID_H)
	/* the steps around the draws: 100 of each, every batch run three times, the fastest counted */
	log_line("bench: the RSX's time for 100 steps of a kind (each batch: its commands written first, then handed over at once)");
	for (test = 0; test < 10 && !failed; test++) {
		static const char *const name[10] = {
			"nothing but the set-up (a target, a texture, two labels)", "a draw of one 8 x 8 rectangle",
			"the texture set again + a draw", "the target switched (prediction <-> middle surface) + a draw",
			"the same target set again + a draw", "a draw + label and wait for it (the row's sync)",
			"the fragment program switched (W2 <-> W2A) + a draw", "a marker (two target switches, the ramp texture, a draw)",
			"the target switched + a draw + the sync", "a label + a draw" };

		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(0);
			taps(0);
			PRED_TGT();
			REF_TEX();
			for (i = 0; i < N && test; i++) {
				if (test == 2)
					REF_TEX();
				if (test == 3 || test == 8) {
					if (i & 1)
						MID_TGT();
					else
						PRED_TGT();
				}
				if (test == 4)
					PRED_TGT();
				if (test == 6)
					fp_load(i & 1);
				if (test == 9)
					CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
				if (test == 7) {
					if (marker_draw(0, 0) != 0)
						goto out;
				} else if (draw_list(&one, 1) != 0)
					goto out;
				if (test == 5 || test == 8) {
					sync_id++;
					CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_SYNC, sync_id));
					CAP("rsxSetWaitLabel", re_set_wait_label(ctx, LAB_SYNC, sync_id));
				}
			}
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		if (test == 0)
			base = best;
		log_line("bench:   %-62s %8.0f us%s", name[test], best, "");
		if (test)
			log_line("bench:     = %.2f us each beyond the set-up", (best - base) / N);
	}
	/* the pixels: 1280 x 704 of the prediction surface (0.90 million pixels) as rectangles of one size */
	log_line("bench: 1280 x 704 pixels (4 x 4: 1280 x 352) as rectangles of one size, one draw call");
	for (test = 0; test < 26 && !failed; test++) {
		static const struct {
			const char *name;
			int w, h, pitch, rh, vert, avg, pos, src;   /* avg: the program (fpx); src 1: the middle surface as the texture */
		} T[26] = {
			{ "one rectangle 1280 x 704, horizontal 8 taps", 1280, 704, 704, 704, 0, 0, 5, 0 },
			{ "64 x 64", 64, 64, 64, 704, 0, 0, 5, 0 },
			{ "32 x 32", 32, 32, 32, 704, 0, 0, 5, 0 },
			{ "16 x 16", 16, 16, 16, 704, 0, 0, 5, 0 },
			{ "8 x 8", 8, 8, 8, 704, 0, 0, 5, 0 },
			{ "4 x 4", 4, 4, 4, 352, 0, 0, 5, 0 },
			{ "8 x 15 every 16 rows (a middle pass's shape)", 8, 15, 16, 704, 0, 0, 5, 0 },
			{ "16 x 23 every 24 rows", 16, 23, 24, 696, 0, 0, 5, 0 },
			{ "16 x 16, vertical taps", 16, 16, 16, 704, 1, 0, 5, 0 },
			{ "16 x 16, the copy kernel", 16, 16, 16, 704, 0, 0, 0, 0 },
			{ "16 x 16 out of the middle surface (1280 x 256)", 16, 16, 16, 256, 1, 0, 5, 1 },
			{ "one rectangle 1280 x 704, W2A (the average)", 1280, 704, 704, 704, 0, 1, 5, 0 },
			{ "16 x 16, W2A", 16, 16, 16, 704, 0, 1, 5, 0 },
			{ "8 x 8, W2A", 8, 8, 8, 704, 0, 1, 5, 0 },
			{ "one rectangle 1280 x 704, W3 (short)", 1280, 704, 704, 704, 0, 2, 5, 0 },
			{ "16 x 16, W3", 16, 16, 16, 704, 0, 2, 5, 0 },
			{ "8 x 8, W3", 8, 8, 8, 704, 0, 2, 5, 0 },
			{ "one rectangle 1280 x 704, W3A (short average)", 1280, 704, 704, 704, 0, 3, 5, 0 },
			{ "16 x 16, W3A", 16, 16, 16, 704, 0, 3, 5, 0 },
			{ "8 x 8, W3A", 8, 8, 8, 704, 0, 3, 5, 0 },
			{ "one rectangle 1280 x 704, W3T (texels snapped)", 1280, 704, 704, 704, 0, 4, 5, 0 },
			{ "16 x 16, W3T", 16, 16, 16, 704, 0, 4, 5, 0 },
			{ "8 x 8, W3T", 8, 8, 8, 704, 0, 4, 5, 0 },
			{ "one rectangle 1280 x 704, W3TA (average)", 1280, 704, 704, 704, 0, 5, 5, 0 },
			{ "16 x 16, W3TA", 16, 16, 16, 704, 0, 5, 5, 0 },
			{ "one rectangle 1280 x 704, W3K (taps snapped)", 1280, 704, 704, 704, 0, 6, 5, 0 } };
		const u32 n = bench_tile(r, 1280, T[test].rh, T[test].w, T[test].h, T[test].pitch, T[test].src ? 0 : by,
		                         T[test].src ? 3 : by, 0, T[test].pos);
		const double px = (double)n * T[test].w * T[test].h;

		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(T[test].avg);
			taps(T[test].vert);
			PRED_TGT();
			if (T[test].src)
				tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
			else
				REF_TEX();
			if (draw_list(r, n) != 0)
				goto out;
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   %-48s %6u rectangles, %8.0f us: %.2f ns a pixel, %.3f us a rectangle (set-up %.0f us taken off)",
		         T[test].name, (unsigned)n, best, (best - base) * 1000.0 / px, (best - base) / n, base);
	}
	/* two triangles through the index buffer against one quad a rectangle (W3T) */
	log_line("bench: rectangles as two indexed triangles / as one quad (W3T, 1280 x 704; 4 x 4: 1280 x 352)");
	for (test = 0; test < 10 && !failed; test++) {
		static const int S[5] = { 64, 32, 16, 8, 4 };
		const int sz = S[test >> 1], q = test & 1;
		const u32 n = bench_tile(r, 1280, sz == 4 ? 352 : 704, sz, sz, sz, by, by, 0, 5);

		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(4);
			taps(0);
			PRED_TGT();
			REF_TEX();
			quads = q;
			i = draw_list(r, n);
			quads = 0;
			if (i != 0)
				goto out;
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   %2d x %2d as %-9s %6u rectangles, %8.0f us: %.2f ns a pixel, %.3f us a rectangle", sz, sz,
		         q ? "quads:" : "triangles:", (unsigned)n, best, (best - base) * 1000.0 / ((double)n * sz * sz), (best - base) / n);
	}
	/* the target's width: 16 x 16 rectangles into surfaces of 64 .. 2560 pixels a row (3.44 found the RSX twice as slow
	   into surfaces 64 wide; the source rows stay within the reference's first 1024) */
	log_line("bench: 16 x 16 rectangles (W3T) into a target of another width (main memory, pitch = width)");
	for (test = 0; test < 6 && !failed; test++) {
		static const int W[6] = { 2560, 1024, 512, 256, 128, 64 };
		const int w = W[test], h = 901120 / w > 4032 ? 4032 : (901120 / w) & ~15;
		u32 n = 0;
		int x, y;

		for (y = 0; y + 16 <= h; y += 16)
			for (x = 0; x + 16 <= w; x += 16, n++) {
				r[n].x0 = (int16_t)x;
				r[n].y0 = (int16_t)y;
				r[n].x1 = (int16_t)(x + 16);
				r[n].y1 = (int16_t)(y + 16);
				r[n].dx = (int16_t)by;
				r[n].dy = (int16_t)(by + (y & 1023) - y);
				r[n].set = 0;
				r[n].pos = 5;
				r[n].pad[0] = r[n].pad[1] = 0;
			}
		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(4);
			taps(0);
			target_set(RE_LOCATION_CELL, pred0, (u32)w, (u16)w, (u16)h);
			REF_TEX();
			if (draw_list(r, n) != 0)
				goto out;
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   target %4d x %4d: %6u rectangles, %8.0f us: %.2f ns a pixel", w, h, (unsigned)n, best,
		         (best - base) * 1000.0 / ((double)n * 256.0));
	}
	/* 4.9: where does the packed prediction surface cost the RSX its time (4.8: the same rows take 1.8 - 2.8 times as
	   long, the one-pass rectangles up to 5 times)? One superblock row's worth of rectangles of one size (40
	   superblocks), W3T, by target shape, source and order:
	     target: F frame-shaped (2560 wide, superblock c at x = c * 64); P packed, pitch 64 (superblock c at
	             y = c * 96); L the same in RSX memory; a number: packed with that pitch (the surface that wide)
	     source: f the reference at the block's frame position (as the decoder reads); s the reference, a strip 64 wide;
	             m the middle surface (RSX memory) at the block's frame position
	     order:  0 superblock after superblock (as the decoder draws); 1 block row after block row across all superblocks */
	log_line("bench: one superblock row of rectangles by target shape, source and order (W3T)");
	for (test = 0; test < 38 && !failed; test++) {
		/* (4.9: none of the first 22 differs - a dense single draw is as fast into the packed surface. 4.10: the same
		   rectangles spread over several draws (draws; each takes the rectangles a hash gives it, the texture set
		   again before it), only a part of them drawn at all (keep 1 of n), vertical taps) */
		static const struct {
			const char *name;
			int tgt, pitch, src, sz, order;     /* tgt 0 F, 1 P (main memory), 2 L (RSX memory) */
			int draws, keep, vert;
		} Q[38] = {
			{ "F f  8x8   18 draws", 0, 2560, 0, 8, 0, 18, 1, 0 },
			{ "P f  8x8   18 draws", 1, 64, 0, 8, 0, 18, 1, 0 },
			{ "F f  8x8   1 of 3 kept", 0, 2560, 0, 8, 0, 1, 3, 0 },
			{ "P f  8x8   1 of 3 kept", 1, 64, 0, 8, 0, 1, 3, 0 },
			{ "F f  8x8   18 draws, 1 of 3 kept", 0, 2560, 0, 8, 0, 18, 3, 0 },
			{ "P f  8x8   18 draws, 1 of 3 kept", 1, 64, 0, 8, 0, 18, 3, 0 },
			{ "F f  8x8   vertical taps", 0, 2560, 0, 8, 0, 1, 1, 1 },
			{ "P f  8x8   vertical taps", 1, 64, 0, 8, 0, 1, 1, 1 },
			{ "F f  8x8   18 draws, vertical", 0, 2560, 0, 8, 0, 18, 1, 1 },
			{ "P f  8x8   18 draws, vertical", 1, 64, 0, 8, 0, 18, 1, 1 },
			{ "F f  4x4   18 draws", 0, 2560, 0, 4, 0, 18, 1, 0 },
			{ "P f  4x4   18 draws", 1, 64, 0, 4, 0, 18, 1, 0 },
			{ "L f  8x8   18 draws (target in RSX memory)", 2, 64, 0, 8, 0, 18, 1, 0 },
			{ "P m  8x8   18 draws (source in RSX memory)", 1, 64, 2, 8, 0, 18, 1, 0 },
			{ "packed, pitch 256  f 8x8 18 draws", 1, 256, 0, 8, 0, 18, 1, 0 },
			{ "packed, pitch 2560 f 8x8 18 draws", 1, 2560, 0, 8, 0, 18, 1, 0 },
			{ "F f  8x8   (the decoder, frame-shaped)", 0, 2560, 0, 8, 0 },
			{ "P f  8x8   (the decoder, packed)", 1, 64, 0, 8, 0 },
			{ "P s  8x8   (source: a strip)", 1, 64, 1, 8, 0 },
			{ "P m  8x8   (source in RSX memory)", 1, 64, 2, 8, 0 },
			{ "F s  8x8", 0, 2560, 1, 8, 0 },
			{ "F m  8x8", 0, 2560, 2, 8, 0 },
			{ "L f  8x8   (target in RSX memory)", 2, 64, 0, 8, 0 },
			{ "L m  8x8", 2, 64, 2, 8, 0 },
			{ "P f  8x8   order: block rows across", 1, 64, 0, 8, 1 },
			{ "F f  8x8   order: block rows across", 0, 2560, 0, 8, 1 },
			{ "P f  16x16", 1, 64, 0, 16, 0 },
			{ "F f  16x16", 0, 2560, 0, 16, 0 },
			{ "P f  32x32", 1, 64, 0, 32, 0 },
			{ "F f  32x32", 0, 2560, 0, 32, 0 },
			{ "P f  64x64", 1, 64, 0, 64, 0 },
			{ "F f  64x64", 0, 2560, 0, 64, 0 },
			{ "P f  4x4", 1, 64, 0, 4, 0 },
			{ "F f  4x4", 0, 2560, 0, 4, 0 },
			{ "packed, pitch 128   f 8x8", 1, 128, 0, 8, 0 },
			{ "packed, pitch 256   f 8x8", 1, 256, 0, 8, 0 },
			{ "packed, pitch 1024  f 8x8", 1, 1024, 0, 8, 0 },
			{ "packed, pitch 2560  f 8x8", 1, 2560, 0, 8, 0 } };
		const int sz = Q[test].sz, nb = 64 / sz, draws = Q[test].draws ? Q[test].draws : 1, keep = Q[test].keep ? Q[test].keep : 1;
		u32 n = 0, drawn = 0, j;
		int c, bx, bq, a0, a1, dd;

		for (a0 = 0; a0 < (Q[test].order ? nb : 40); a0++)
			for (a1 = 0; a1 < (Q[test].order ? 40 : nb); a1++)
				for (bx = 0; bx < nb && n < MAXR; bx++, n++) {
					int x, y, sx, sy;

					c = Q[test].order ? a1 : a0;
					bq = Q[test].order ? a0 : a1;
					x = Q[test].tgt ? bx * sz : c * 64 + bx * sz;
					y = Q[test].tgt ? c * 96 + bq * sz : bq * sz;
					sx = Q[test].src == 1 ? bx * sz + by : c * 64 + bx * sz + (Q[test].src == 2 ? 0 : by);
					sy = Q[test].src == 1 ? ((c * 96 + bq * sz) & 1023) + by : bq * sz + (Q[test].src == 2 ? 3 : by + 200);
					r[n].x0 = (int16_t)x;
					r[n].y0 = (int16_t)y;
					r[n].x1 = (int16_t)(x + sz);
					r[n].y1 = (int16_t)(y + sz);
					r[n].dx = (int16_t)(sx - x);
					r[n].dy = (int16_t)(sy - y);
					r[n].set = 0;
					r[n].pos = 5;
					r[n].pad[0] = r[n].pad[1] = 0;
				}
		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(4);
			taps(Q[test].vert);
			if (Q[test].tgt == 0)
				target_set(RE_LOCATION_CELL, pred0, (u32)aw, (u16)aw, surf_h);
			else if (Q[test].tgt == 1)
				target_set(RE_LOCATION_CELL, pred0, (u32)Q[test].pitch, (u16)Q[test].pitch, 3840);
			else
				target_set(RE_LOCATION_RSX, store_off, 64, 64, 3840);
			drawn = 0;
			for (dd = 0; dd < draws; dd++) {
				u32 m = 0;

				if (Q[test].src == 2)
					tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
				else
					REF_TEX();
				if (draws == 1 && keep == 1) {
					if (draw_list(r, n) != 0)
						goto out;
					drawn = n;
					break;
				}
				for (j = 0; j < n; j++) {       /* (the rectangles of this draw, in their order) */
					const u32 hsh = (j * 2654435761u) >> 8;

					if (hsh % (u32)draws == (u32)dd && (hsh >> 8) % (u32)keep == 0)
						r2[m++] = r[j];
				}
				if (m && draw_list(r2, m) != 0)
					goto out;
				drawn += m;
			}
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   %-42s %5u rectangles, %7.0f us: %.3f us a rectangle, %.2f ns a pixel", Q[test].name, (unsigned)drawn, best,
		         (best - base) / (drawn ? drawn : 1), (best - base) * 1000.0 / ((double)(drawn ? drawn : 1) * sz * sz));
	}
	/* 4.12: a surface copied out of RSX memory into main memory with one rectangle (the CP program) */
	log_line("bench: one rectangle copied from RSX memory into main memory (CP)");
	for (test = 0; test < 3 && !failed; test++) {
		static const struct {
			const char *name;
			int w, h;
		} C[3] = { { "64 x 3840 (a packed superblock row)", 64, 3840 }, { "2560 x 96", 2560, 96 }, { "2560 x 256 out of the middle surface", 2560, 256 } };
		static const f32 zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const rsxmc_rect cr = { 0, 0, (int16_t)C[test].w, (int16_t)C[test].h, 0, 0, 0, 0, { 0, 0 } };

		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			target_set(RE_LOCATION_CELL, pred0, (u32)C[test].w, (u16)C[test].w, (u16)C[test].h);
			fp_load(FP_COPY);
			taps(0);
			CAP("rsxSetVertexProgramParameter", re_set_vp_parameter(ctx, vp, vc_tap[0], zero));
			if (test == 2)
				tex_set(RE_LOCATION_RSX, mid_off, (u16)aw, RSXMC_MID_H, (u32)aw);
			else
				tex_set(RE_LOCATION_RSX, ploc_off, (u16)C[test].w, (u16)C[test].h, (u32)C[test].w);
			if (draw_list(&cr, 1) != 0)
				goto out;
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   %-42s %7.0f us: %.3f ns a pixel", C[test].name, best, (best - base) * 1000.0 / ((double)C[test].w * C[test].h));
	}
	/* 4.11: scattered rectangles (18 draws) are slow into the packed surface in main memory (4.10: 1.59 us a rectangle
	   against 0.24 frame-shaped; 0.15 with the target in RSX memory). By the target's pitch, height and cell shape:
	     geo 0: the frame-shaped band (64 rows, superblock c at x = c * 64)
	     geo 1: cells of 64 x 96 one below the other (superblock c at y = c * 96)
	     geo 2: cells of 384 x 16 one below the other: the superblock's luma as four strips of 64 x 16 side by side
	     geo 3: cells of 64 x 96 side by side in rows of `per` cells (the surface per * 64 wide) */
	log_line("bench: 8 x 8 rectangles in 18 draws by the target's pitch and shape (W3T, main memory)");
	for (test = 0; test < 38 && !failed; test++) {
		/* geo 4: strip cells of 384 x 16 (luma as 4 strips of 64 x 16), `per` side by side; geo 5: of 192 x 32 (luma as 2
		   strips of 64 x 32) - 4.13: the shapes a superblock could have for 16 / 32 list elements instead of 128 */
		static const struct {
			const char *name;
			int geo, pitch, nsb, per;
		} G[38] = {
			{ "strips 384x16, 6 a row, pitch 2304", 4, 2304, 40, 6 },
			{ "strips 384x16, 6 a row, pitch 2560", 4, 2560, 40, 6 },
			{ "strips 192x32, 12 a row, pitch 2304", 5, 2304, 40, 12 },
			{ "strips 192x32, 13 a row, pitch 2560", 5, 2560, 40, 13 },
			{ "strips 384x16, 6 a row, pitch 2304: 460 cells", 4, 2304, 460, 6 },
			{ "strips 192x32, 12 a row, pitch 2304: 460 cells", 5, 2304, 460, 12 },
			{ "band, pitch 2560: 460 superblocks (half a frame)", 6, 2560, 460, 40 },
			{ "strips 384x16, 5 a row, pitch 1920", 4, 1920, 40, 5 },
			{ "cells 64x96 stacked, pitch 64", 1, 64, 40, 0 },
			{ "cells 64x96 stacked, pitch 128", 1, 128, 40, 0 },
			{ "cells 64x96 stacked, pitch 192", 1, 192, 40, 0 },
			{ "cells 64x96 stacked, pitch 256", 1, 256, 40, 0 },
			{ "cells 64x96 stacked, pitch 320", 1, 320, 40, 0 },
			{ "cells 64x96 stacked, pitch 384", 1, 384, 40, 0 },
			{ "cells 64x96 stacked, pitch 512", 1, 512, 40, 0 },
			{ "cells 64x96 stacked, pitch 640", 1, 640, 40, 0 },
			{ "cells 64x96 stacked, pitch 1024", 1, 1024, 40, 0 },
			{ "cells 64x96 stacked, pitch 1280", 1, 1280, 40, 0 },
			{ "cells 64x96 stacked, pitch 2048", 1, 2048, 40, 0 },
			{ "cells 64x96 stacked, pitch 2560", 1, 2560, 40, 0 },
			{ "cells 64x96 stacked, pitch 4096", 1, 4096, 40, 0 },
			{ "band, pitch 2560", 0, 2560, 40, 0 },
			{ "band, pitch 2624", 0, 2624, 40, 0 },
			{ "band, pitch 4096", 0, 4096, 40, 0 },
			{ "cells 64x96 stacked, pitch 64: 20 cells", 1, 64, 20, 0 },
			{ "cells 64x96 stacked, pitch 64: 10 cells", 1, 64, 10, 0 },
			{ "cells 64x96 stacked, pitch 64: 5 cells", 1, 64, 5, 0 },
			{ "cells 64x96 stacked, pitch 64: 2 cells", 1, 64, 2, 0 },
			{ "strips 384x16 stacked, pitch 384", 2, 384, 40, 0 },
			{ "strips 384x16 stacked, pitch 512", 2, 512, 40, 0 },
			{ "strips 384x16 stacked, pitch 2560", 2, 2560, 40, 0 },
			{ "cells 64x96, 2 a row, pitch 128", 3, 128, 40, 2 },
			{ "cells 64x96, 4 a row, pitch 256", 3, 256, 40, 4 },
			{ "cells 64x96, 5 a row, pitch 320", 3, 320, 40, 5 },
			{ "cells 64x96, 8 a row, pitch 512", 3, 512, 40, 8 },
			{ "cells 64x96, 10 a row, pitch 640", 3, 640, 40, 10 },
			{ "cells 64x96, 20 a row, pitch 1280", 3, 1280, 40, 20 },
			{ "cells 64x96, 40 a row, pitch 2560", 3, 2560, 40, 40 } };
		const int geo = G[test].geo, nsb = G[test].nsb, per = G[test].per;
		u32 n = 0, drawn = 0, j;
		int c, bx, bq, dd, tw, th;

		for (c = 0; c < nsb; c++)
			for (bq = 0; bq < 8; bq++)
				for (bx = 0; bx < 8; bx++, n++) {
					const int x = geo == 0 ? c * 64 + bx * 8 : geo == 2 ? (bq >> 1) * 64 + bx * 8 : geo == 3 ? (c % per) * 64 + bx * 8 :
					              geo == 4 ? (c % per) * 384 + (bq >> 1) * 64 + bx * 8 : geo == 5 ? (c % per) * 192 + (bq >> 2) * 64 + bx * 8 :
					              geo == 6 ? (c % 40) * 64 + bx * 8 : bx * 8;
					const int y = geo == 0 ? bq * 8 : geo == 2 ? c * 16 + (bq & 1) * 8 : geo == 3 ? (c / per) * 96 + bq * 8 :
					              geo == 4 ? (c / per) * 16 + (bq & 1) * 8 : geo == 5 ? (c / per) * 32 + (bq & 3) * 8 :
					              geo == 6 ? (c / 40) * 64 + bq * 8 : c * 96 + bq * 8;
					const int sx = (c % 40) * 64 + bx * 8 + by, sy = (c / 40) * 64 + bq * 8 + by;

					r[n].x0 = (int16_t)x;
					r[n].y0 = (int16_t)y;
					r[n].x1 = (int16_t)(x + 8);
					r[n].y1 = (int16_t)(y + 8);
					r[n].dx = (int16_t)(sx - x);
					r[n].dy = (int16_t)(sy - y);
					r[n].set = 0;
					r[n].pos = 5;
					r[n].pad[0] = r[n].pad[1] = 0;
				}
		tw = G[test].pitch;
		th = geo == 0 || geo == 6 ? 2208 : geo == 2 ? nsb * 16 : geo == 3 ? ((nsb + per - 1) / per) * 96 :
		     geo == 4 ? ((nsb + per - 1) / per) * 16 : geo == 5 ? ((nsb + per - 1) / per) * 32 : nsb * 96;
		for (rep = 0, best = 1e30; rep < 3; rep++) {
			if (cmd_room(400000) != 0)
				goto out;
			fp_load(4);
			taps(0);
			target_set(RE_LOCATION_CELL, pred0, (u32)tw, (u16)(geo == 0 || geo == 6 ? 2560 : tw), (u16)th);
			drawn = 0;
			for (dd = 0; dd < 18; dd++) {
				u32 m = 0;

				REF_TEX();
				for (j = 0; j < n; j++)
					if (((j * 2654435761u) >> 8) % 18u == (u32)dd)
						r2[m++] = r[j];
				if (m && draw_list(r2, m) != 0)
					goto out;
				drawn += m;
			}
			fp_load(0);
			t = bench_run();
			if (t < 0)
				goto out;
			if (t < best)
				best = t;
		}
		log_line("bench:   %-46s %4d x %4d: %5u rectangles, %7.0f us: %.3f us a rectangle", G[test].name, geo == 0 || geo == 6 ? 2560 : tw, th,
		         (unsigned)drawn, best, (best - base) / (drawn ? drawn : 1));
	}
#undef REF_TEX
#undef PRED_TGT
#undef MID_TGT
out:
	tv9h_unlock();
	tv9h_predicting(0);
	free(r);
}
#endif

#ifdef RSX_MC_BENCH
/* the RSX's time for the first `rows` superblock rows of a frame as the decoder draws them (emit_row), once into the
   frame-shaped prediction surface and once into the packed one. lists: bit 0 the middle passes and what reads them,
   bit 1 the one-pass rectangles, both: everything; planes: bit p plane p */
static u32 bench_lists = 3, bench_planes = 7;

void rsx_mc_bench_rows(const rsx_mc_desc *d, int rows, const char *what)
{
	mc_slot *s = &slots[0];
	const int packed0 = geom.packed, ploc0 = ploc_use, strips0 = geom.strips;
	int layout, rep, row, k, p, l;

	if (!ready || running) {
		log_line("bench rows: not ready");
		return;
	}
	s->d = *d;
	s->base = 0x52000000u;
	for (k = 0; k < 3; k++)
		for (p = 0; p < 3; p++) {
			const u32 st = p ? d->stride_c : d->stride_y, b = (u32)(p ? geom.border_c : geom.border_y);
			const uint64_t vis = p == 0 ? d->ref[k].y : p == 1 ? d->ref[k].u : d->ref[k].v;

			s->tex[k][p] = offset_of((const void *)(uintptr_t)(vis - (uint64_t)b * st - b));
		}
	tv9h_predicting(1);
	tv9h_lock();
	if (cmd_room(8192) == 0) {
		programs_load();
		bench_run();
	}
	tv9h_unlock();
	for (layout = 0; layout < 4 && !failed; layout++) {
		double best = 1e30, t = 0;
		u32 rects = 0, vbytes = 0;

		if (layout == 2)
			continue;                       /* (packed + copy: 4.12 measured it, far too slow) */
		geom.packed = layout == 1 || layout == 2;
		geom.strips = layout == 3 ? RSXMC_STRIP_H : 0;
		ploc_use = layout == 2;
		for (rep = 0; rep < 3 && !failed; rep++) {
			const u64 v0 = st_vtx_bytes;

			tv9h_lock();
			k = cmd_room(700000);
			tv9h_unlock();
			if (k != 0)
				break;
			rects = 0;
			bench_noflush = 1;
			for (row = 0; row < rows && !failed; row++) {
				row_lists L;

				rsxmc_reset(&s->L);
				d->fn(d->user, row, &s->L);
				for (l = 0; l < RSXMC_NLIST; l++) {
					const int cls = l / 28, i = l % 28;   /* (the lists of a class: 9 middle, 9 + 9 one-pass, 1 from the middle) */
					const int mid = i < 9 || i == 27, pl = i == 27 ? -1 : (i % 9) / 3;

					(void)cls;
					if (l < 84 && (!(bench_lists >> (mid ? 0 : 1) & 1) || (pl >= 0 && !(bench_planes >> pl & 1))))
						s->L.n[l] = 0;
					L.r[l] = s->L.r[l];
					rects += s->L.n[l];
				}
				L.n = s->L.n;
				L.rects = s->L.rects;
				s->row = row;
				if (emit_row(s, 0, &L) != 0)
					break;
			}
			bench_noflush = 0;
			vbytes = (u32)(st_vtx_bytes - v0);
			tv9h_lock();
			t = bench_run();
			tv9h_unlock();
			if (t < 0)
				break;
			if (t < best)
				best = t;
		}
		log_line("bench rows: %-28s %-13s %2d rows, %6u rectangles (%4u KB of vertices): %8.0f us = %.3f us a rectangle",
		         what, layout == 3 ? "CELLS 192x32" : layout == 2 ? "PACKED+copy" : layout ? "PACKED" : "frame-shaped", rows, (unsigned)rects, (unsigned)(vbytes >> 10), best,
		         rects ? best / rects : 0.0);
	}
	geom.packed = packed0;
	geom.strips = strips0;
	ploc_use = ploc0;
	s->row = 0;
	tv9h_predicting(0);
}

void rsx_mc_bench_filter(unsigned lists, unsigned planes)
{
	bench_lists = lists;
	bench_planes = planes;
}
#endif

/* ---- a run, a frame ----------------------------------------------------------------------------------------------- */

int rsx_mc_start(void)
{
	if (!ready || running)
		return -1;
	st_rows = st_rects = st_frames = st_us_build = st_us_emit = st_us_ring = st_us_cb = st_cmd_bytes = st_vtx_bytes = 0;
	st_restarts = st_ringwaits = st_row_max = st_idle = st_draws = 0;
	st_two_rows = 0;
	st_lat_us = st_lat_n = st_lat_max = st_fly_bytes = st_fly_n = st_fly_max = 0;
	failed = stop_flag = 0;
	test_rows = 0;
	if ((compact != compact_want || two != (two_want && !strips_want)) && vp_select() != 0)   /* (these may change from run to run) */
		return -1;
	shortp = short_want;
	geom.packed = packed_want != 0;
	geom.strips = strips_want ? RSXMC_STRIP_H : 0;
	ploc_use = packed_want == 2;
	st_copies = 0;
	tv9h_predicting(1);
	tv9h_lock();
	if (cmd_room(4096) == 0) {
		int k;

		programs_load();
		/* every slot's marker made void (byte 0 = 0: no row): one left by an earlier run could carry the bits of this
		   run's first frame in the slot (3.42e: a run without markers in between did that) */
		for (k = 0; k < RSX_MC_SLOTS && !failed; k++)
			if (marker_draw(k, 0) != 0)
				fail("the markers could not be reset");
		if (!failed && cmd_room(64) == 0) {
			s64 t0;

			CAP("rsxSetWriteBackendLabel", re_set_write_backend_label(ctx, LAB_CHUNK, ++chunk_id));
			unlabeled = 0;
			rxp_flush(ctx);
			t0 = now_us();
			while (*(vu32 *)rxp_label(LAB_CHUNK) != chunk_id && now_us() - t0 < WAIT_US)
				tv9p_sleep_us(100);
			tv9p_sleep_us(2000);                   /* (the pixels reach main memory some 10 us after the label) */
		}
		rxp_flush(ctx);
	}
	tv9h_unlock();
	if (failed) {
		tv9h_predicting(0);
		return -1;
	}
	running = 1;
	unlabeled = 0;
	thread_b = tv9p_thread_start(builder, NULL, 1000, "teersxb");
	if (!thread_b) {
		running = 0;
		tv9h_predicting(0);
		log_line("rsx_mc: the builder thread did not start");
		return -1;
	}
	thread = tv9p_thread_start(worker, NULL, 1000, "teersx");
	if (!thread) {
		stop_flag = 1;
		tv9p_thread_join(thread_b);
		stop_flag = 0;
		running = 0;
		tv9h_predicting(0);
		log_line("rsx_mc: the worker thread did not start");
		return -1;
	}
	return 0;
}

void rsx_mc_stop(void)
{
	int k;

	test_stall_end(1);                      /* (a test's stopped RSX goes on, whatever happens) */
	if (!running)
		return;
	stop_flag = 1;
	__asm__ volatile("sync" ::: "memory");
	tv9p_thread_join(thread);
	tv9p_thread_join(thread_b);
	running = 0;
	stop_flag = 0;
	for (k = 0; k < RSX_MC_SLOTS; k++)
		slots[k].state = 0;
	test_fail_rows = test_stall_rows = 0;  /* (a test's injection is for one run) */
	if (!failed) {                          /* everything drawn before the display takes the RSX again */
		const s64 t0 = now_us();

		tv9h_lock();
		rxp_flush(ctx);
		while (*(vu32 *)rxp_label(LAB_CHUNK) != chunk_id && now_us() - t0 < WAIT_US)
			tv9p_sleep_us(50);
		tv9h_unlock();
	}
	tv9h_predicting(0);
}

int rsx_mc_failed(void)
{
	return failed;
}

void rsx_mc_give_up(void)
{
	int i, k;

	if (!pred_mem || !ready)
		return;
	if (!failed)
		fail("given up by the decoder");
	for (i = 0; i < RSX_MC_SLOTS; i++) {
		u8 *const m = pred_mem + pred_bytes - MARK_BYTES + (size_t)i * 64;
		const u8 b = (u8)((slot_gen[i] & 7) << 5 | 31);      /* (a marker's first byte: the slot's count, the row + 1) */

		for (k = 0; k < 16; k++)
			m[k] = (u8)(b + k);
		*(vu32 *)rxp_label((u8)(LAB_ROW0 + i)) = slots[i].base + 30;
	}
	__asm__ volatile("sync" ::: "memory");
}

int rsx_mc_begin(const rsx_mc_desc *d, pix_frame *pred, uint32_t *pitch, uint64_t *lab_ea, uint32_t *lab_base)
{
	mc_slot *s;
	int i, k, p;

	if (!running || failed)
		return -1;
	for (i = 0; i < RSX_MC_SLOTS && slots[i].state != 0; i++)
		;
	if (i == RSX_MC_SLOTS)
		return -1;
	s = &slots[i];
	s->d = *d;
	s->row = 0;
	s->b_row = 0;
	s->built = 0;
	s->r_wr = 0;
	s->abort = 0;
	s->seen = -1;
	slot_gen[i]++;
	lab_gen += 256;
	s->base = lab_gen;
	*(vu32 *)rxp_label((u8)(LAB_ROW0 + i)) = s->base - 1;
	for (k = 0; k < 3; k++)
		for (p = 0; p < 3; p++) {
			const u32 st = p ? d->stride_c : d->stride_y, b = (u32)(p ? geom.border_c : geom.border_y);
			const uint64_t vis = p == 0 ? d->ref[k].y : p == 1 ? d->ref[k].u : d->ref[k].v;

			s->tex[k][p] = offset_of((const void *)(uintptr_t)(vis - (uint64_t)b * st - b));
		}
	pred->y = (uint64_t)(uintptr_t)(pred_mem + (size_t)i * surf_bytes);
	pred->u = pred->y + (uint64_t)geom.aw * (uint64_t)geom.ah;
	pred->v = pred->u + (uint64_t)geom.aw / 2;
	pred->pad = 0;
	*pitch = geom.strips ? (uint32_t)RSXMC_STRIP_PITCH : geom.packed ? 64u : (uint32_t)geom.aw;
	*lab_ea = (uint64_t)(uintptr_t)rxp_label((u8)(LAB_ROW0 + i));
	*lab_base = s->base;
	__asm__ volatile("sync" ::: "memory");
	s->state = 1;
	return i;
}

void rsx_mc_end(int slot)
{
	mc_slot *s = &slots[slot];

	if (s->state == 1) {                    /* given up before its rows were through */
		const s64 t0 = now_us();

		s->abort = 1;
		while (s->state == 1 && running && now_us() - t0 < WAIT_US)
			tv9p_sleep_us(50);
	}
	{                                       /* (the builder thread lets go of it) */
		const s64 t0 = now_us();

		s->abort = 1;
		__asm__ volatile("sync" ::: "memory");
		while (s->building && s->b_row < s->d.sb_rows && running && now_us() - t0 < WAIT_US)
			tv9p_sleep_us(50);
	}
	__asm__ volatile("sync" ::: "memory");
	s->state = 0;
}

const uint8_t *rsx_mc_surface(int slot)
{
	return pred_mem + (size_t)slot * surf_bytes;
}

int rsx_mc_rows_done(int slot)
{
	return (int)(s32)(*(vu32 *)rxp_label((u8)(LAB_ROW0 + slot)) - slots[slot].base);
}

int rsx_mc_row_flags(int slot, int row)
{
	return slots[slot].rowflag[row & 63];
}

#ifdef RSX_EMIT_CHECK
/* (rsx_emit_check.h) the emitter against librsx with made-up arguments: also the functions no decoding run calls */
void rsx_mc_emit_selftest(void)
{
	static const u8 *const v[4] = { tv9_prog_vp_w2, tv9_prog_vp_w2c, tv9_prog_vp_w2a, tv9_prog_vp_w2ca };
	static const u8 *const f[8] = { tv9_prog_w2, tv9_prog_w2a, tv9_prog_w3, tv9_prog_w3a, tv9_prog_w3t, tv9_prog_w3ta, tv9_prog_w3k, tv9_prog_cp };

	rec_selftest((const re_vp *const *)(const void *)v, 4, (const re_fp *const *)(const void *)f, 8);
}
#endif

void rsx_mc_report(const char *name)
{
	const double f = st_frames ? (double)st_frames : 1.0;

	log_line("%s: RSX prediction row by row: %u frames, %u rows, %.0f rectangles a frame (most in a row: %u), %u draws; PPU a "
	         "frame: %.2f ms rectangles, %.2f ms vertices and commands (%.0f KB of %d-byte vertices, %.1f KB of commands); command buffer "
	         "started over %u times (%.2f ms in all), vertex ring full %u times (%.2f ms in all); the worker idled %u times; a "
	         "row's label %.0f us after its commands (max %.0f); the RSX's backlog at a row's start %.0f KB of vertices (max "
	         "%.0f); row labels %s a target switch; blocks with two references: %s (%u rows had some)%s", name,
	         (unsigned)st_frames, (unsigned)st_rows, st_rects / f, (unsigned)st_row_max, (unsigned)st_draws, st_us_build / 1e3 / f,
	         st_us_emit / 1e3 / f, st_vtx_bytes / 1024.0 / f, compact ? 16 : 48, st_cmd_bytes / 1024.0 / f, (unsigned)st_restarts, st_us_cb / 1e3,
	         (unsigned)st_ringwaits, st_us_ring / 1e3, (unsigned)st_idle, st_lat_n ? (double)st_lat_us / st_lat_n : 0.0,
	         (double)st_lat_max, st_fly_n ? st_fly_bytes / 1024.0 / st_fly_n : 0.0, st_fly_max / 1024.0, row_marker ? "and markers, none after" : row_flush ? "after" : "WITHOUT",
	         two ? "the RSX's" : "the pixel SPUs'", (unsigned)st_two_rows, failed ? "; FAILED" : "");
	log_line("%s: prediction surface: %s; the RSX %s for its back end before it reads what it drew", name,
	         geom.strips ? "cells of 192 x 32 a superblock (a list of 32 elements)" : !geom.packed ? "frame-shaped" : ploc_use ? "PACKED (a superblock in one piece), drawn in RSX memory and copied a row at "
	         "a time" : "PACKED (a superblock in one piece), drawn directly", sync_on ? "waits" : "does NOT wait");
	log_line("%s: RSX fragment programs: %s", name, shortp == 1 ? "W3 / W3A (nothing snapped: 14 / 19 instructions)" :
	         shortp == 2 ? "W3T / W3TA (texels snapped: 18 / 23 instructions)" : shortp == 3 ? "W3K / W2A (taps snapped: 18 / 30 "
	         "instructions)" : "W2 / W2A (22 / 30 instructions)");
	if (re_overflow)
		log_line("%s: RSX commands: %u calls found no room in the command buffer and wrote nothing  <== ERROR", name, re_overflow);
#ifdef RSX_EMIT_CHECK
	rec_report(name);
#endif
}

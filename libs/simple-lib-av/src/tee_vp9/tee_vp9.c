/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/tee_vp9.c; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9 - see tee_vp9.h. The decoder's PPU side: what the test application's run_pipe / take_frame / pixel_done
   do (test.c), without its checks and statistics and without the RSX, behind the interface of tee_vp9.h:

     tvp9_feed (the app's thread)  -> frames in the input ring
     the decoder's thread          -> entropy jobs to the entropy SPEs, their records into superblock-row order,
                                      pixel jobs to the pixel SPEs, finished pictures into the output queue
     tvp9_get_picture / _release   <- the app's display thread

   The three talk through single-writer rings (one side writes an index, the other reads it) and tv9p_wake; there
   is no lock. Everything else is the decoder thread's alone.

   Only tv9_plat.h is used from the system, so the same file builds with PSL1GHT and with Sony's SDK. */
#include "tee_vp9.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tv9_plat.h"
#include "vp9_job.h"
#include "vp9_pix_job.h"

#define MAX_ENT       2
#define MAX_PIX       4
#define DEPTH         3              /* entropy slots: frames sent to the entropy SPEs and not taken yet */
#define NTASK_MAX     7
#define NFB_MAX       16
#define INQ           256            /* frames fed and not taken from the entropy stage yet */
#define DQ            64             /* shown frames from their entropy answer to the output queue */
#define OUTQ          16             /* finished pictures the app has not fetched */
#define RELQ          64
#define REC_CAP       (5u << 20)     /* one frame's records (the largest key frame of the test streams: 4.6 MB) */
#define OUT_CAP       (9u << 19)     /* per entropy SPE and slot: its part of one frame's records */
#define MV_BYTES      (40 * 40 * VP9_MV_SB_BYTES)
#define STORE_ROWS    4              /* the superblock store (PIXJ_SBSTORE): a ring of 4 superblock rows ... */
#define STORE_COLS    40             /* ... up to 2560 pixels wide, in the end of the task's records buffer */
#define STORE_BYTES   ((uint32_t)STORE_ROWS * STORE_COLS * PIX_STORE_SB)
#define REC_TILE_HI   0x5449         /* a tile's record in the entropy stage's output (vp9_dec.h: VP9_REC_TILE_*) */
#define REC_TILE_LO   0x4C45
#define FRAME_KEY     0

typedef struct {
	uint8_t *mem;
	pix_frame pf;
	int refs;                        /* reference slots holding it */
	int writing, readers;            /* a pixel job decoding it; running ones reading it */
	int disp;                        /* pictures of it on their way to the app or held by it */
} fbuf;

typedef struct {
	pix_job job, job2;               /* the whole frame or its left half; its right half */
	int fb, refs[3], used, left, tiles;
	uint32_t na, nb, oa;
	int seam;
} ptask;

typedef struct {
	uint32_t off, size, padded;      /* in the input ring */
	int64_t pts;
} in_frame;

typedef struct {
	int fb, key, gen, index;
	int64_t pts;
} out_pic;

struct tvp9 {
	tvp9_config c;
	char err[200];
	volatile int failed, hung;
	void *thread;
	volatile int cmd_close, cmd_flush, flush_ack, eos, drained;
	volatile int gen;
	int nent, npix, lead, ntask, nfb;

	/* the stream's size, known from its first frame after tvp9_open / tvp9_flush */
	int geo, w, h, aw, ah, sy, sc, sbr;
	size_t ysize, csize, fb_cap, pic_bytes;
	uint8_t *pic_mem;
	int pic_own;
	fbuf fb[NFB_MAX];
	uint8_t *progmem;                /* per frame buffer 256 bytes: its progress, and a split frame's two */

	/* the entropy stage */
	uint8_t *outmem[MAX_ENT][DEPTH];
	uint32_t *rowoffmem[DEPTH];
	uint8_t *mvmem[2], *xchgmem;
	int e_next, e_done, e_ans[MAX_ENT], e_got[DEPTH];   /* frames sent / taken / answered by SPE k; answers in a slot */
	uint32_t slot_in[DEPTH];         /* a slot's frame in inq */
	uint32_t seq, fseq;
	int reset_next;                  /* the next frame starts a stream (JOB_RESET) */

	/* the pixel stage */
	ptask tasks[NTASK_MAX];
	uint8_t *recbuf[NTASK_MAX], *lfm[NTASK_MAX];
	int q[2 * NTASK_MAX], qn;        /* pixel jobs ready, in decode order: task * 2 + part */
	int run[MAX_PIX];                /* the job each pixel SPE runs (task * 2 + part), -1: none */
	int slotfb[8];

	/* shown frames, in the order fed */
	struct {
		int fb, key;
		int64_t pts;
	} dq[DQ];
	uint32_t dq_r, dq_w;
	int pic_index;
	out_pic outq[OUTQ];
	volatile uint32_t out_w, out_r;  /* the decoder writes, the app reads */
	struct {
		int fb, gen;
	} relq[RELQ];
	volatile uint32_t rel_w, rel_r;  /* the app writes, the decoder reads */

	/* the input: frames in a byte ring, oldest first */
	uint8_t *inbuf;
	uint32_t in_cap, ring_w;         /* ring_w: the feeder's */
	in_frame inq[INQ];
	volatile uint32_t in_w;          /* the feeder's: frames fed */
	volatile uint32_t in_done;       /* the decoder's: frames whose bytes are free again */
	uint32_t in_sent;                /* the decoder's: frames sent to the entropy stage */
	int started;                     /* the feeder's: a key frame has come */

	/* statistics (the decoder thread writes, anyone reads) */
	volatile int st_frames, st_pictures, st_dropped;
	volatile double st_entropy_ticks, st_pixel_ticks, st_busy_ticks;

	vp9_job jobs[MAX_ENT][DEPTH];
	vp9_result results[MAX_ENT][DEPTH];
	pix_result mres[MAX_PIX];
} __attribute__((aligned(128)));

static void fail(tvp9 *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void fail(tvp9 *d, const char *fmt, ...)
{
	va_list ap;

	if (d->failed)
		return;
	va_start(ap, fmt);
	vsnprintf(d->err, sizeof(d->err), fmt, ap);
	va_end(ap);
	tv9p_sync();
	d->failed = 1;
	tv9p_log("TEE VP9 stops: %s", d->err);
}

/* ---- configuration and memory ------------------------------------------------------------------------------ */

void tvp9_default(tvp9_config *c)
{
	memset(c, 0, sizeof(*c));
	c->entropy_spes = 2;
	c->pixel_spes = 4;
	c->max_width = 2560;
	c->max_height = 1440;
	c->pictures = 10;
	c->input_bytes = 6u << 20;
}

static int cfg_lead(const tvp9_config *c)
{
	return c->lead >= 1 && c->lead <= DEPTH ? c->lead : DEPTH;
}

static int cfg_tasks(const tvp9_config *c)
{
	return c->tasks >= 2 && c->tasks <= NTASK_MAX ? c->tasks : 5;
}

static int cfg_pictures(const tvp9_config *c)
{
	return c->pictures < 6 ? 6 : c->pictures > NFB_MAX ? NFB_MAX : c->pictures;
}

/* a frame buffer of a w x h stream: every plane aligned to 64 / 32 with its border */
static size_t fb_size(int w, int h, int *sy, int *sc, size_t *ysize, size_t *csize)
{
	const int aw = (w + 63) & ~63, ah = (h + 63) & ~63;
	const int y = aw + 2 * PIX_BORDER_Y, c = aw / 2 + 2 * PIX_BORDER_C;
	const size_t ys = (size_t)y * (size_t)(ah + 2 * PIX_BORDER_Y), cs = (size_t)c * (size_t)(ah / 2 + 2 * PIX_BORDER_C);

	if (sy)
		*sy = y;
	if (sc)
		*sc = c;
	if (ysize)
		*ysize = ys;
	if (csize)
		*csize = cs;
	return (ys + 2 * cs + 127) & ~(size_t)127;
}

size_t tvp9_picture_memory_size(const tvp9_config *c)
{
	const size_t one = fb_size(c->max_width, c->max_height, NULL, NULL, NULL, NULL);

	return (one * (size_t)cfg_pictures(c) + 0xFFFFF) & ~(size_t)0xFFFFF;
}

static size_t lfm_bytes(const tvp9_config *c)
{
	return (size_t)((c->max_width + 63) / 64) * (size_t)((c->max_height + 63) / 64) * PIX_LFM_BYTES + 256;
}

size_t tvp9_memory_size(const tvp9_config *c)
{
	const int nent = c->entropy_spes == 1 ? 1 : 2;

	return sizeof(tvp9) + (size_t)nent * DEPTH * OUT_CAP + (size_t)DEPTH * VP9_ROWOFF_BYTES + 2 * (size_t)MV_BYTES + VP9_XCHG_BYTES +
	       (size_t)cfg_tasks(c) * (REC_CAP + lfm_bytes(c)) + (size_t)NFB_MAX * 256 + (c->input_bytes ? c->input_bytes : 6u << 20);
}

/* ---- a frame's header, as far as a player needs it ----------------------------------------------------------- */

typedef struct {
	const uint8_t *p;
	size_t size, bit;
} bitr;

static int rb(bitr *r, int n)
{
	int v = 0;

	while (n--) {
		const size_t byte = r->bit >> 3;

		v = v << 1 | (byte < r->size ? (r->p[byte] >> (7 - (r->bit & 7))) & 1 : 0);
		r->bit++;
	}
	return v;
}

int tvp9_probe(const void *block, size_t size, int *width, int *height, int *profile)
{
	bitr r = { block, size, 0 };
	int prof;

	if (!block || size < 1 || rb(&r, 2) != 2)
		return -1;
	prof = rb(&r, 1);
	prof |= rb(&r, 1) << 1;
	if (prof > 2)
		prof += rb(&r, 1);
	if (profile)
		*profile = prof;
	if (rb(&r, 1))                      /* show_existing_frame */
		return 0;
	if (rb(&r, 1) != FRAME_KEY)
		return 0;
	rb(&r, 2);                          /* show_frame, error_resilient_mode */
	if (size < 10 || rb(&r, 8) != 0x49 || rb(&r, 8) != 0x83 || rb(&r, 8) != 0x42)
		return -1;
	if (prof >= 2)
		rb(&r, 1);                      /* 10 or 12 bit */
	if (rb(&r, 3) != 7) {               /* the colour space; 7 is sRGB */
		rb(&r, 1);                      /* the colour range */
		if (prof == 1 || prof == 3)
			rb(&r, 3);                  /* subsampling x, y, a reserved bit */
	} else if (prof == 1 || prof == 3)
		rb(&r, 1);
	if (width)
		*width = rb(&r, 16) + 1;
	else
		rb(&r, 16);
	if (height)
		*height = rb(&r, 16) + 1;
	return 1;
}

/* a block's frames (libvpx's superframe index: a hidden frame in front of a shown one); returns how many */
static int superframe(const uint8_t *data, size_t size, const uint8_t **frames, size_t *sizes, int max)
{
	if (size > 0) {
		const uint8_t marker = data[size - 1];

		if ((marker & 0xe0) == 0xc0) {
			const int n = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1;
			const size_t index_sz = 2 + (size_t)mag * (size_t)n;

			if (size >= index_sz && data[size - index_sz] == marker && n <= max) {
				const uint8_t *x = data + size - index_sz + 1;
				size_t off = 0;
				int i, j;

				for (i = 0; i < n; i++) {
					size_t this_sz = 0;

					for (j = 0; j < mag; j++)
						this_sz |= (size_t)*x++ << (j * 8);
					if (this_sz > size - index_sz - off)
						return 0;               /* the index does not fit the block */
					frames[i] = data + off;
					sizes[i] = this_sz;
					off += this_sz;
				}
				return n;
			}
		}
	}
	frames[0] = data;
	sizes[0] = size;
	return 1;
}

/* ---- feeding (the app's thread) ---------------------------------------------------------------------------- */

/* room for n bytes in the ring: its offset, or 0xFFFFFFFF. The live frames are inq[in_done .. in_w), oldest first */
static uint32_t ring_room(tvp9 *d, uint32_t w, uint32_t n, uint32_t first_live, int any_live)
{
	if (!any_live)
		return n <= d->in_cap ? 0 : 0xFFFFFFFFu;
	if (w > first_live) {               /* the live bytes are [first_live, w) */
		if (w + n <= d->in_cap)
			return w;
		return n < first_live ? 0 : 0xFFFFFFFFu;
	}
	return w + n < first_live ? w : 0xFFFFFFFFu;   /* they wrap: [first_live, end) and [0, w); w == first_live is full */
}

int tvp9_feed(tvp9 *d, const void *block, size_t size, int64_t pts)
{
	const uint8_t *frames[8];
	size_t sizes[8];
	uint32_t offs[8], w, done, nlive;
	int n, i, m = 0;

	if (!d || d->failed || !block || size == 0 || size > d->in_cap / 2)
		return -1;
	n = superframe(block, size, frames, sizes, 8);
	if (n < 1)
		return -1;
	if (!d->started) {                  /* a stream starts with a key frame: what comes before is dropped */
		if (tvp9_probe(frames[0], sizes[0], NULL, NULL, NULL) != 1) {
			d->st_dropped++;
			return 1;
		}
		d->started = 1;
	}
	done = d->in_done;
	nlive = d->in_w - done;
	if (nlive + (uint32_t)n > INQ)
		return 0;
	w = nlive ? d->ring_w : 0;
	for (i = 0; i < n; i++) {
		const uint32_t padded = ((uint32_t)sizes[i] + 127) & ~127u;
		uint32_t at;

		if (!sizes[i])
			continue;
		at = ring_room(d, w, padded, nlive ? d->inq[done % INQ].off : (m ? offs[0] : 0), nlive || m);
		if (at == 0xFFFFFFFFu)
			return 0;                   /* (nothing is written yet: the ring is as it was) */
		offs[m] = at;
		frames[m] = frames[i];
		sizes[m] = sizes[i];
		w = at + padded;
		m++;
	}
	for (i = 0; i < m; i++) {
		in_frame *f = &d->inq[(d->in_w + (uint32_t)i) % INQ];
		const uint32_t padded = ((uint32_t)sizes[i] + 127) & ~127u;

		memcpy(d->inbuf + offs[i], frames[i], sizes[i]);
		memset(d->inbuf + offs[i] + sizes[i], 0, padded - sizes[i]);
		f->off = offs[i];
		f->size = (uint32_t)sizes[i];
		f->padded = padded;
		f->pts = pts;
	}
	d->ring_w = w;
	tv9p_sync();
	d->in_w += (uint32_t)m;
	tv9p_sync();
	tv9p_wake();
	return 1;
}

void tvp9_end_of_stream(tvp9 *d)
{
	if (!d)
		return;
	d->eos = 1;
	tv9p_sync();
	tv9p_wake();
}

/* ---- the decoder's thread ------------------------------------------------------------------------------------ */

static pix_prog *prog_of(tvp9 *d, int fb)
{
	return (pix_prog *)(d->progmem + (size_t)fb * 256);
}

static pix_prog *recp_of(tvp9 *d, int fb)
{
	return (pix_prog *)(d->progmem + (size_t)fb * 256 + 128);
}

static pix_prog *halfp_of(tvp9 *d, int fb)
{
	return (pix_prog *)(d->progmem + (size_t)fb * 256 + 144);
}

/* the frame buffers for a w x h stream, inside the picture block */
static int geometry(tvp9 *d, int w, int h)
{
	size_t one;

	if (w < 16 || h < 16 || w > d->c.max_width || h > d->c.max_height || ((w + 63) & ~63) > STORE_COLS * 64) {
		fail(d, "a stream of %d x %d: the decoder is set up for %d x %d at most", w, h, d->c.max_width, d->c.max_height);
		return -1;
	}
	one = fb_size(w, h, &d->sy, &d->sc, &d->ysize, &d->csize);
	if (one > d->fb_cap) {
		fail(d, "a %d x %d frame needs %u bytes, a picture buffer has %u", w, h, (unsigned)one, (unsigned)d->fb_cap);
		return -1;
	}
	d->w = w;
	d->h = h;
	d->aw = (w + 63) & ~63;
	d->ah = (h + 63) & ~63;
	d->sbr = d->ah / 64;
	for (int k = 0; k < d->nfb; k++) {
		uint8_t *m = d->pic_mem + (size_t)k * d->fb_cap;

		memset(m, 0, one);
		d->fb[k].mem = m;
		d->fb[k].pf.y = (uint64_t)tv9p_ea(m + (size_t)PIX_BORDER_Y * (size_t)d->sy + PIX_BORDER_Y);
		d->fb[k].pf.u = (uint64_t)tv9p_ea(m + d->ysize + (size_t)PIX_BORDER_C * (size_t)d->sc + PIX_BORDER_C);
		d->fb[k].pf.v = d->fb[k].pf.u + d->csize;
		d->fb[k].pf.pad = 0;
	}
	d->geo = 1;
	tv9p_log("TEE VP9: a stream of %d x %d (frame buffers %d x %d, strides %d / %d)", w, h, d->aw, d->ah, d->sy, d->sc);
	return 0;
}

/* entropy slot s's records (all SPEs' parts) superblock row by superblock row, as the pixel SPU takes them: every
   row's part of each tile (tile raster order) after a copy of the tile's record; side 0 / 1: only the tiles left of /
   from superblock column seam on (-1: all); returns the bytes, 0 when they do not add up or do not fit. *seam_out
   (when not NULL): the tile column start that splits the records' bytes most evenly, 0 with fewer than two tile
   columns. (test.c's rows_order) */
static uint32_t rows_order(tvp9 *d, int s, uint8_t *dst, uint32_t cap, int seam, int side, int *seam_out)
{
	struct {
		const uint8_t *mem;
		uint32_t start, end;
		int r0, r1, c0;
	} t[VP9_RES_TILES];
	int starts[VP9_RES_TILES], nst = 0;
	uint32_t cbytes[VP9_RES_TILES];
	uint32_t n = 0;

	memset(t, 0, sizeof(t));
	for (int k = 0; k < d->nent; k++) {
		const vp9_result *er = &d->results[k][s];

		if (er->ntiles > VP9_RES_TILES)
			return 0;
		for (uint32_t i = 0; i < er->ntiles; i++) {
			const int idx = er->tile_idx[i];
			const uint32_t start = er->tile_off[i], end = i + 1 < er->ntiles ? er->tile_off[i + 1] : er->out_bytes;
			const uint32_t *tr = (const uint32_t *)(d->outmem[k][s] + start);

			if (idx >= VP9_RES_TILES || t[idx].mem || ((start | end) & 15) || end < start + 32 || end > OUT_CAP ||
			    tr[0] != ((uint32_t)REC_TILE_HI << 16 | REC_TILE_LO))
				return 0;
			t[idx].mem = d->outmem[k][s];
			t[idx].start = start;
			t[idx].end = end;
			t[idx].r0 = (int)(tr[2] >> 16) >> 3;
			t[idx].r1 = ((int)(tr[2] & 0xFFFF) + 7) >> 3;
			t[idx].c0 = (int)(tr[1] >> 16) >> 3;
		}
	}
	if (seam_out) {
		uint32_t total = 0, best = 0xFFFFFFFFu;

		for (int idx = 0; idx < VP9_RES_TILES; idx++) {
			int k;

			if (!t[idx].mem)
				continue;
			for (k = 0; k < nst && starts[k] != t[idx].c0; k++)
				;
			if (k == nst)
				starts[nst++] = t[idx].c0;
		}
		for (int a = 1; a < nst; a++)
			for (int b = a; b > 0 && starts[b - 1] > starts[b]; b--) {
				const int x = starts[b];

				starts[b] = starts[b - 1];
				starts[b - 1] = x;
			}
		*seam_out = nst >= 2 ? starts[nst / 2] : 0;
		for (int k = 0; k < nst; k++)
			cbytes[k] = 0;
		for (int idx = 0; idx < VP9_RES_TILES; idx++)
			for (int k = 0; t[idx].mem && k < nst; k++)
				if (starts[k] == t[idx].c0)
					cbytes[k] += t[idx].end - t[idx].start, total += t[idx].end - t[idx].start;
		for (int k = 1, left = (int)cbytes[0]; k < nst; left += (int)cbytes[k], k++) {
			const uint32_t dd = (uint32_t)abs(2 * left - (int)total);

			if (dd < best)
				best = dd, *seam_out = starts[k];
		}
	}
	for (int r = 0; r < d->sbr; r++)
		for (int idx = 0; idx < VP9_RES_TILES; idx++) {
			const uint32_t *ro = d->rowoffmem[s] + idx * (VP9_ROWOFF_TILE / 4);
			uint32_t a, b;

			if (!t[idx].mem || r < t[idx].r0 || r >= t[idx].r1 || (side == 0 && t[idx].c0 >= seam) || (side == 1 && t[idx].c0 < seam))
				continue;
			a = ro[r];
			b = r + 1 < t[idx].r1 ? ro[r + 1] : t[idx].end;
			if (a < t[idx].start + 16 || b > t[idx].end || a > b || ((a | b) & 15) || n + 16 + (b - a) > cap)
				return 0;
			memcpy(dst + n, t[idx].mem + t[idx].start, 16);
			memcpy(dst + n + 16, t[idx].mem + a, b - a);
			n += 16 + (b - a);
		}
	return n;
}

/* a shown frame on its way out: its buffer stays until the app releases the picture */
static int show_later(tvp9 *d, int fb, int key, int64_t pts)
{
	if (d->dq_w - d->dq_r >= DQ) {
		fail(d, "too many shown frames wait for their pixels");
		return -1;
	}
	d->dq[d->dq_w % DQ].fb = fb;
	d->dq[d->dq_w % DQ].key = key;
	d->dq[d->dq_w % DQ].pts = pts;
	d->dq_w++;
	d->fb[fb].disp++;
	return 0;
}

/* the entropy stage is done with frame i (slot i % DEPTH): its pixel job(s). 1 taken, 0 not yet (no frame buffer or no
   records buffer free: one comes with a pixel job's end or a released picture), -1 the decoder stops. (test.c's
   take_frame) */
static int take_frame(tvp9 *d, int i)
{
	const int s = i % DEPTH;
	const vp9_result *er = &d->results[0][s];
	const int64_t pts = d->inq[d->slot_in[s] % INQ].pts;
	int fb, t, refs[3], intra, seam = 0;
	uint32_t n;
	ptask *tk;
	pix_job *j;
	pix_prog *pp;

	for (int k = 0; k < d->nent; k++)
		if (d->results[k][s].status != RES_OK) {
			const vp9_result *e = &d->results[k][s];

			fail(d, "frame %d: the entropy stage reports status %u, error %u (%u, %u)%s", d->st_frames, (unsigned)e->status,
			     (unsigned)e->error, (unsigned)e->error_a, (unsigned)e->error_b,
			     e->status == RES_OUT_FULL ? " - its records do not fit" : e->status == RES_PARSE_ERROR ? " - not a stream it decodes" : "");
			return -1;
		}
	if (er->show_existing) {
		fb = d->slotfb[er->existing_idx & 7];
		if (fb < 0) {
			fail(d, "frame %d shows the empty reference slot %u", d->st_frames, (unsigned)er->existing_idx);
			return -1;
		}
		return show_later(d, fb, 0, pts) == 0 ? 1 : -1;
	}
	intra = er->frame_type == FRAME_KEY || er->intra_only;
	if (!d->geo) {
		if (er->frame_type != FRAME_KEY) {
			fail(d, "the stream does not start with a key frame");
			return -1;
		}
		if (geometry(d, (int)er->width, (int)er->height) != 0)
			return -1;
	}
	if ((int)er->width != d->w || (int)er->height != d->h) {
		fail(d, "frame %d is %u x %u, the stream began with %d x %d (a new size needs tvp9_flush)", d->st_frames, (unsigned)er->width,
		     (unsigned)er->height, d->w, d->h);
		return -1;
	}
	for (fb = 0; fb < d->nfb && (d->fb[fb].refs || d->fb[fb].writing || d->fb[fb].readers || d->fb[fb].disp); fb++)
		;
	for (t = 0; t < d->ntask && d->tasks[t].used; t++)
		;
	if (fb == d->nfb || t == d->ntask)
		return 0;
	for (int k = 0; k < 3; k++) {
		refs[k] = intra ? -1 : d->slotfb[er->ref_frame_idx[k] & 7];
		if (!intra && refs[k] < 0) {
			fail(d, "frame %d: reference slot %u is empty", d->st_frames, (unsigned)er->ref_frame_idx[k]);
			return -1;
		}
	}
	tk = &d->tasks[t];
	n = rows_order(d, s, d->recbuf[t], REC_CAP, 0, -1, &seam);
	if (!n) {
		fail(d, "frame %d: its records do not go into superblock-row order (%u + %u bytes)", d->st_frames,
		     (unsigned)d->results[0][s].out_bytes, d->nent > 1 ? (unsigned)d->results[1][s].out_bytes : 0);
		return -1;
	}
	/* split by tile columns: the left half's records first, the right half's after them (the same bytes in all) */
	tk->tiles = d->npix >= 2 && seam > 0;
	if (tk->tiles) {
		const uint32_t na = rows_order(d, s, d->recbuf[t], REC_CAP, seam, 0, NULL);
		const uint32_t oa = (na + 127) & ~127u;
		const uint32_t nb = oa < REC_CAP ? rows_order(d, s, d->recbuf[t] + oa, REC_CAP - oa, seam, 1, NULL) : 0;

		if (!na || !nb || na + nb != n) {
			fail(d, "frame %d: its records do not split at superblock column %d (%u + %u of %u bytes)", d->st_frames, seam,
			     (unsigned)na, (unsigned)nb, (unsigned)n);
			return -1;
		}
		tk->na = na;
		tk->nb = nb;
		tk->oa = oa;
		tk->seam = seam;
	}
	j = &tk->job;
	pp = prog_of(d, fb);
	memset(j, 0, sizeof(*j));
	j->flags = (er->refresh_frame_flags ? PIXJ_BORDERS : 0) | PIXJ_LF_NEW;
	j->width = er->width;
	j->height = er->height;
	j->aw = (uint32_t)d->aw;
	j->ah = (uint32_t)d->ah;
	j->stride_y = (uint32_t)d->sy;
	j->stride_c = (uint32_t)d->sc;
	j->intra_frame = (uint32_t)intra;
	j->lossless = er->lossless;
	j->lf_level = er->lf_level;
	j->lf_sharpness = er->lf_sharpness;
	j->lf_delta_enabled = er->lf_delta_enabled;
	for (int k = 0; k < 4; k++)
		j->lf_ref_deltas[k] = er->lf_ref_deltas[k];
	j->lf_mode_deltas[0] = er->lf_mode_deltas[0];
	j->lf_mode_deltas[1] = er->lf_mode_deltas[1];
	j->records_bytes = n;
	j->records_ea = (uint64_t)tv9p_ea(d->recbuf[t]);
	if (!d->c.no_store) {               /* (set before the halves' jobs are copied from this one) */
		const uint32_t used = tk->tiles ? tk->oa + tk->nb : n;

		if (used + 256 <= REC_CAP - STORE_BYTES) {
			j->flags |= PIXJ_SBSTORE;
			j->store_ea = (uint64_t)tv9p_ea(d->recbuf[t] + REC_CAP - STORE_BYTES);
			j->store_rows = STORE_ROWS;
		}
	}
	j->lfm_ea = (uint64_t)tv9p_ea(d->lfm[t]);
	j->cur = d->fb[fb].pf;
	j->prog_ea = (uint64_t)tv9p_ea(pp);
	for (int k = 0; k < 3; k++) {
		j->ref[k] = d->fb[intra ? fb : refs[k]].pf;
		j->ref_prog_ea[k] = intra ? 0 : (uint64_t)tv9p_ea(prog_of(d, refs[k]));
		tk->refs[k] = refs[k];
		if (refs[k] >= 0)
			d->fb[refs[k]].readers++;
	}
	tk->fb = fb;
	tk->used = 1;
	tk->left = 1 + tk->tiles;
	pp->rows = 0;                       /* nothing of this buffer's new frame is final yet */
	pp->lim_y = pp->lim_c = PIX_LIM_NONE;
	d->fb[fb].writing = 1;
	d->q[d->qn++] = t * 2;
	if (tk->tiles) {
		pix_prog *lp = recp_of(d, fb), *rp = halfp_of(d, fb);

		lp->rows = rp->rows = 0;
		lp->lim_y = lp->lim_c = rp->lim_y = rp->lim_c = 0;
		tk->job2 = *j;
		j->flags |= PIXJ_HALF_A;
		j->records_bytes = tk->na;
		j->rec_ea = (uint64_t)tv9p_ea(lp);
		j->peer_ea = (uint64_t)tv9p_ea(rp);
		j->seam = (uint32_t)tk->seam;
		j->prog_ea = 0;                 /* the right half's: it goes last in every row */
		tk->job2.flags |= PIXJ_HALF_B;
		tk->job2.records_ea = j->records_ea + tk->oa;
		tk->job2.records_bytes = tk->nb;
		tk->job2.rec_ea = (uint64_t)tv9p_ea(rp);
		tk->job2.peer_ea = (uint64_t)tv9p_ea(lp);
		tk->job2.seam = (uint32_t)tk->seam;
		d->q[d->qn++] = t * 2 + 1;
	}
	for (int k = 0; k < 8; k++)
		if (er->refresh_frame_flags & (1 << k)) {
			if (d->slotfb[k] >= 0)
				d->fb[d->slotfb[k]].refs--;
			d->slotfb[k] = fb;
			d->fb[fb].refs++;
		}
	if (er->show_frame && show_later(d, fb, er->frame_type == FRAME_KEY, pts) != 0)
		return -1;
	return 1;
}

static void pixel_done(tvp9 *d, int p, uint32_t word)
{
	const int e = d->run[p], t = e >> 1, part = e & 1;
	const pix_result *pr = &d->mres[p];
	ptask *tk;

	if (e < 0 || (word >> 8) != ((part ? d->tasks[t].job2.seq : d->tasks[t].job.seq) & 0xFFFF)) {
		fail(d, "an unexpected answer 0x%x from pixel SPE %d", (unsigned)word, p);
		d->hung = 1;
		return;
	}
	tk = &d->tasks[t];
	d->run[p] = -1;
	d->st_pixel_ticks += pr->ticks_total;
	if (pr->status != PIX_OK)
		fail(d, "frame %d: pixel SPE %d reports status %u (part %d of %d)", d->st_frames, p, (unsigned)pr->status, part + 1,
		     1 + tk->tiles);
	if (--tk->left > 0)
		return;                         /* the other half still runs */
	d->fb[tk->fb].writing = 0;
	for (int k = 0; k < 3; k++)
		if (tk->refs[k] >= 0)
			d->fb[tk->refs[k]].readers--;
	tk->used = 0;
	d->st_frames++;
}

/* everything that can go on without an answer; returns 1 when something did */
static int advance(tvp9 *d)
{
	const int stopping = d->failed || d->cmd_flush || d->cmd_close;
	int moved = 0;

	/* pictures the app is done with */
	while (d->rel_r != d->rel_w) {
		const int fb = d->relq[d->rel_r % RELQ].fb, gen = d->relq[d->rel_r % RELQ].gen;

		if (gen == d->gen && fb >= 0 && fb < d->nfb && d->fb[fb].disp > 0)
			d->fb[fb].disp--;
		d->rel_r++;
		moved = 1;
	}
	if (stopping) {
		/* nothing new starts. A frame whose left half runs must get its right half too (the halves wait for each
		   other's rows); the frames behind it in the queue have not begun and are dropped. Frames a running job
		   reads were sent before it, so they are running or done */
		if (d->qn > 0)
			d->qn = (d->q[0] & 1) ? 1 : 0;
	}
	/* entropy: up to `lead` frames ahead, every SPE its share of the tile columns */
	while (!stopping && d->in_sent != d->in_w && d->e_next - d->e_done < d->lead &&
	       (!d->c.max_pictures || d->st_pictures + (int)(d->dq_w - d->dq_r) < d->c.max_pictures)) {
		const int slot = d->e_next % DEPTH;
		const in_frame *f = &d->inq[d->in_sent % INQ];

		d->fseq++;
		for (int k = 0; k < d->nent; k++) {
			vp9_job *j = &d->jobs[k][slot];

			memset(j, 0, sizeof(*j));
			j->seq = ++d->seq;
			j->flags = d->reset_next ? JOB_RESET : 0;
			j->frame_size = f->size;
			j->data_size = f->padded;
			j->data_ea = (uint64_t)tv9p_ea(d->inbuf + f->off);
			j->out_ea = (uint64_t)tv9p_ea(d->outmem[k][slot]);
			j->out_cap = OUT_CAP;
			j->part = (uint32_t)k;
			j->parts = (uint32_t)d->nent;
			j->fseq = d->fseq;
			j->mv_ea[0] = (uint64_t)tv9p_ea(d->mvmem[0]);
			j->mv_ea[1] = (uint64_t)tv9p_ea(d->mvmem[1]);
			j->xchg_ea = (uint64_t)tv9p_ea(d->xchgmem);
			j->result_ea = (uint64_t)tv9p_ea(&d->results[k][slot]);
			j->mv_bytes = MV_BYTES;
			j->rowoff_ea = (uint64_t)tv9p_ea(d->rowoffmem[slot]);
		}
		d->reset_next = 0;
		d->slot_in[slot] = d->in_sent;
		tv9p_sync();
		for (int k = 0; k < d->nent; k++)
			if (tv9p_spe_send(k, tv9p_ea(&d->jobs[k][slot])) != 0) {
				fail(d, "entropy SPE %d's mailbox refused a job", k);
				d->hung = 1;
				return 1;
			}
		d->e_next++;
		d->in_sent++;
		moved = 1;
	}
	/* the frames all entropy SPEs have answered, in order */
	while (!stopping && !d->failed && d->e_done < d->e_next && d->e_got[d->e_done % DEPTH] == d->nent) {
		const int rc = take_frame(d, d->e_done);
		double e = 0;

		if (rc <= 0)
			break;
		for (int k = 0; k < d->nent; k++)
			if (d->results[k][d->e_done % DEPTH].ticks_total > e)
				e = d->results[k][d->e_done % DEPTH].ticks_total;
		d->st_entropy_ticks += e;
		d->e_got[d->e_done % DEPTH] = 0;
		d->e_done++;
		tv9p_sync();
		d->in_done++;                   /* the frame's bytes are free (the input goes on in order) */
		moved = 1;
	}
	/* pixel jobs to the free pixel SPEs, in decode order */
	for (int p = 0; p < d->npix && d->qn > 0 && !d->hung; p++)
		if (d->run[p] < 0) {
			const int e = d->q[0];
			ptask *tk = &d->tasks[e >> 1];
			pix_job *j = (e & 1) ? &tk->job2 : &tk->job;

			memmove(d->q, d->q + 1, sizeof(int) * (size_t)(--d->qn));
			j->seq = ++d->seq;
			j->result_ea = (uint64_t)tv9p_ea(&d->mres[p]);
			tv9p_sync();
			if (tv9p_spe_send(d->nent + p, tv9p_ea(j)) != 0) {
				fail(d, "pixel SPE %d's mailbox refused a job", p);
				d->hung = 1;
				return 1;
			}
			d->run[p] = e;
			moved = 1;
		}
	/* the shown frames whose pixels are done, in order, to the app */
	while (!stopping && d->dq_r != d->dq_w && !d->fb[d->dq[d->dq_r % DQ].fb].writing && d->out_w - d->out_r < OUTQ) {
		out_pic *o = &d->outq[d->out_w % OUTQ];

		o->fb = d->dq[d->dq_r % DQ].fb;
		o->key = d->dq[d->dq_r % DQ].key;
		o->pts = d->dq[d->dq_r % DQ].pts;
		o->gen = d->gen;
		o->index = d->pic_index++;
		d->dq_r++;
		tv9p_sync();
		d->out_w++;
		d->st_pictures++;
		moved = 1;
	}
	return moved;
}

/* SPE jobs out and not answered */
static int outstanding(const tvp9 *d)
{
	int n = 0;

	for (int k = 0; k < d->nent; k++)
		n += d->e_next - d->e_ans[k];
	for (int p = 0; p < d->npix; p++)
		n += d->run[p] >= 0;
	return n;
}

/* a flush: everything fed and not shown is gone, the next frame starts a stream */
static void restart(tvp9 *d)
{
	d->in_sent = d->in_w;
	d->in_done = d->in_w;
	d->e_next = d->e_done = 0;
	for (int k = 0; k < MAX_ENT; k++)
		d->e_ans[k] = 0;
	for (int k = 0; k < DEPTH; k++)
		d->e_got[k] = 0;
	for (int k = 0; k < NTASK_MAX; k++)
		d->tasks[k].used = 0;
	d->qn = 0;
	for (int k = 0; k < NFB_MAX; k++)
		d->fb[k].refs = d->fb[k].writing = d->fb[k].readers = d->fb[k].disp = 0;
	for (int k = 0; k < 8; k++)
		d->slotfb[k] = -1;
	d->dq_r = d->dq_w = 0;
	d->pic_index = 0;
	d->geo = 0;
	d->reset_next = 1;
	d->drained = 0;
	d->st_frames = d->st_pictures = d->st_dropped = 0;
	d->st_entropy_ticks = d->st_pixel_ticks = d->st_busy_ticks = 0;
	d->gen++;                           /* pictures of before are not the app's any more */
	tv9p_sync();
}

static void decoder_thread(void *arg)
{
	tvp9 *d = arg;
	const int timeout_us = (d->c.timeout_ms > 0 ? d->c.timeout_ms : 10000) * 1000;

	for (;;) {
		int spe = 0, r, out;
		uint32_t word = 0;
		uint64_t t0;

		while (advance(d))
			;
		out = outstanding(d);
		if (!out) {
			if (d->cmd_close)
				break;
			if (d->cmd_flush && !d->flush_ack) {
				restart(d);
				d->flush_ack = 1;
				tv9p_sync();
			}
			if (d->eos && !d->drained && !d->cmd_flush && d->in_sent == d->in_w && d->e_done == d->e_next && d->qn == 0 &&
			    d->dq_r == d->dq_w) {
				d->drained = 1;         /* (the pictures in the output queue are still the app's to fetch) */
				tv9p_sync();
			}
		}
		if (d->hung && d->cmd_close)
			break;
		t0 = tv9p_ticks();
		r = tv9p_wait(out && !d->hung ? timeout_us : 20000, &spe, &word);
		if (out)
			d->st_busy_ticks += (double)(tv9p_ticks() - t0);
		if (r == 1) {
			if (spe < d->nent) {
				const int slot = d->e_ans[spe] % DEPTH;

				if (d->e_ans[spe] >= d->e_next || (word >> 8) != (d->jobs[spe][slot].seq & 0xFFFF)) {
					fail(d, "an unexpected answer 0x%x from entropy SPE %d", (unsigned)word, spe);
					d->hung = 1;
				} else {
					d->e_got[slot]++;
					d->e_ans[spe]++;
				}
			} else if (spe < d->nent + d->npix)
				pixel_done(d, spe - d->nent, word);
		} else if (r == 0 && out && !d->hung) {
			fail(d, "an SPE did not answer within %d ms (%d jobs out)", timeout_us / 1000, out);
			d->hung = 1;
		} else if (r < 0 && !d->hung) {
			fail(d, "the event queue failed");
			d->hung = 1;
		}
	}
}

/* ---- open, close, flush -------------------------------------------------------------------------------------- */

static void free_all(tvp9 *d)
{
	for (int k = 0; k < MAX_ENT; k++)
		for (int s = 0; s < DEPTH; s++)
			tv9p_free(d->outmem[k][s]);
	for (int s = 0; s < DEPTH; s++)
		tv9p_free(d->rowoffmem[s]);
	for (int k = 0; k < NTASK_MAX; k++) {
		tv9p_free(d->recbuf[k]);
		tv9p_free(d->lfm[k]);
	}
	tv9p_free(d->mvmem[0]);
	tv9p_free(d->mvmem[1]);
	tv9p_free(d->xchgmem);
	tv9p_free(d->progmem);
	tv9p_free(d->inbuf);
	if (d->pic_own)
		tv9p_free(d->pic_mem);
	tv9p_free(d);
}

tvp9 *tvp9_open(const tvp9_config *c, char *err, int errlen)
{
	tvp9 *d;
	int ok = 1;

#define OPEN_FAIL(...) do { if (err && errlen > 0) snprintf(err, (size_t)errlen, __VA_ARGS__); if (d) free_all(d); return NULL; } while (0)
	d = NULL;
	if (!c || (c->entropy_spes != 1 && c->entropy_spes != 2) || c->pixel_spes < 1 || c->pixel_spes > MAX_PIX ||
	    c->entropy_spes + c->pixel_spes > TV9P_MAX_SPES || c->max_width < 64 || c->max_height < 64 ||
	    c->max_width > STORE_COLS * 64 || c->max_height > 2560)
		OPEN_FAIL("the configuration is not one the decoder takes");
	d = tv9p_alloc(sizeof(*d), 128);
	if (!d)
		OPEN_FAIL("no memory");
	memset(d, 0, sizeof(*d));
	d->c = *c;
	d->nent = c->entropy_spes;
	d->npix = c->pixel_spes;
	d->lead = cfg_lead(c);
	d->ntask = cfg_tasks(c);
	d->nfb = cfg_pictures(c);
	d->in_cap = c->input_bytes ? (c->input_bytes + 127) & ~127u : 6u << 20;
	d->fb_cap = fb_size(c->max_width, c->max_height, NULL, NULL, NULL, NULL);
	d->pic_bytes = tvp9_picture_memory_size(c);
	tv9p_set_log(c->log);
	if (c->picture_memory) {
		if (((uintptr_t)c->picture_memory & 0xFFFFF) || c->picture_memory_bytes < d->pic_bytes)
			OPEN_FAIL("the picture memory must be 1 MB aligned and hold %u bytes", (unsigned)d->pic_bytes);
		d->pic_mem = c->picture_memory;
	} else {
		d->pic_mem = tv9p_alloc(d->pic_bytes, 1 << 20);
		d->pic_own = 1;
	}
	for (int k = 0; k < d->nent; k++)
		for (int s = 0; s < DEPTH; s++)
			ok &= (d->outmem[k][s] = tv9p_alloc(OUT_CAP, 128)) != NULL;
	for (int s = 0; s < DEPTH; s++)
		ok &= (d->rowoffmem[s] = tv9p_alloc(VP9_ROWOFF_BYTES, 128)) != NULL;
	for (int k = 0; k < d->ntask; k++) {
		ok &= (d->recbuf[k] = tv9p_alloc(REC_CAP, 128)) != NULL;
		ok &= (d->lfm[k] = tv9p_alloc(lfm_bytes(c), 128)) != NULL;
	}
	ok &= (d->mvmem[0] = tv9p_alloc(MV_BYTES, 128)) != NULL;
	ok &= (d->mvmem[1] = tv9p_alloc(MV_BYTES, 128)) != NULL;
	ok &= (d->xchgmem = tv9p_alloc(VP9_XCHG_BYTES, 128)) != NULL;
	ok &= (d->progmem = tv9p_alloc((size_t)NFB_MAX * 256, 128)) != NULL;
	ok &= (d->inbuf = tv9p_alloc(d->in_cap, 128)) != NULL;
	if (!ok || !d->pic_mem)
		OPEN_FAIL("no memory: the decoder needs %u MB for its pictures and %u MB beside them",
		          (unsigned)(d->pic_bytes >> 20), (unsigned)(tvp9_memory_size(c) >> 20));
	memset(d->xchgmem, 0, VP9_XCHG_BYTES);
	memset(d->progmem, 0, (size_t)NFB_MAX * 256);
	for (int p = 0; p < MAX_PIX; p++)
		d->run[p] = -1;
	restart(d);
	if (tv9p_init() != 0)
		OPEN_FAIL("the SPU programs or the event queue did not come");
	if (tv9p_spes_start(d->nent, d->npix) != 0) {
		tv9p_shutdown();
		OPEN_FAIL("the SPEs did not start (%d entropy + %d pixel)", d->nent, d->npix);
	}
	d->thread = tv9p_thread_start(decoder_thread, d, 1000, "teevp9");
	if (!d->thread) {
		tv9p_spes_stop(0);
		tv9p_shutdown();
		OPEN_FAIL("the decoder's thread did not start");
	}
#undef OPEN_FAIL
	tv9p_log("TEE VP9 %s: %d entropy + %d pixel SPEs, %d picture buffers (%u MB), %u MB beside them; %d frames ahead, %d tasks",
	         TEE_VP9_VERSION, d->nent, d->npix, d->nfb, (unsigned)(d->pic_bytes >> 20), (unsigned)(tvp9_memory_size(c) >> 20), d->lead,
	         d->ntask);
	return d;
}

void tvp9_close(tvp9 *d)
{
	if (!d)
		return;
	d->cmd_close = 1;
	tv9p_sync();
	tv9p_wake();
	tv9p_thread_join(d->thread);
	tv9p_spes_stop(d->hung);
	tv9p_shutdown();
	free_all(d);
}

int tvp9_flush(tvp9 *d)
{
	int waited = 0;

	if (!d || d->failed)
		return -1;
	d->flush_ack = 0;
	d->cmd_flush = 1;
	tv9p_sync();
	tv9p_wake();
	while (!d->flush_ack && !d->failed && waited < 15000) {
		tv9p_sleep_us(1000);
		waited++;
		if (!(waited & 15))
			tv9p_wake();
	}
	if (!d->flush_ack) {
		if (!d->failed)
			fail(d, "a flush did not finish");
		return -1;
	}
	d->started = 0;
	d->ring_w = 0;
	d->eos = 0;
	d->drained = 0;                     /* (the decoder's thread sets it only while no flush is asked for) */
	tv9p_sync();
	d->cmd_flush = 0;
	tv9p_sync();
	tv9p_wake();
	return 0;
}

/* ---- pictures (the app's display thread) --------------------------------------------------------------------- */

int tvp9_get_picture(tvp9 *d, tvp9_picture *pic)
{
	if (!d || !pic)
		return -1;
	while (d->out_r != d->out_w) {
		const out_pic *o = &d->outq[d->out_r % OUTQ];
		const fbuf *b = &d->fb[o->fb];

		if (o->gen != d->gen) {         /* from before a flush */
			d->out_r++;
			continue;
		}
		pic->y = (const uint8_t *)(uintptr_t)b->pf.y;
		pic->u = (const uint8_t *)(uintptr_t)b->pf.u;
		pic->v = (const uint8_t *)(uintptr_t)b->pf.v;
		pic->stride_y = d->sy;
		pic->stride_c = d->sc;
		pic->width = d->w;
		pic->height = d->h;
		pic->pts = o->pts;
		pic->index = o->index;
		pic->key_frame = o->key;
		pic->offset_y = (uint32_t)(pic->y - d->pic_mem);
		pic->offset_u = (uint32_t)(pic->u - d->pic_mem);
		pic->offset_v = (uint32_t)(pic->v - d->pic_mem);
		pic->out_ = o->fb;
		pic->gen_ = o->gen;
		tv9p_sync();
		d->out_r++;
		tv9p_sync();
		tv9p_wake();                    /* (the output queue has room again) */
		return 1;
	}
	return d->failed || d->drained ? -1 : 0;
}

void tvp9_release_picture(tvp9 *d, const tvp9_picture *pic)
{
	if (!d || !pic || pic->gen_ != d->gen || d->rel_w - d->rel_r >= RELQ)
		return;
	d->relq[d->rel_w % RELQ].fb = pic->out_;
	d->relq[d->rel_w % RELQ].gen = pic->gen_;
	tv9p_sync();
	d->rel_w++;
	tv9p_sync();
	tv9p_wake();
}

void tvp9_copy_i420(const tvp9_picture *pic, uint8_t *dst)
{
	const int cw = (pic->width + 1) / 2, ch = (pic->height + 1) / 2;

	for (int r = 0; r < pic->height; r++, dst += pic->width)
		memcpy(dst, pic->y + (size_t)r * (size_t)pic->stride_y, (size_t)pic->width);
	for (int r = 0; r < ch; r++, dst += cw)
		memcpy(dst, pic->u + (size_t)r * (size_t)pic->stride_c, (size_t)cw);
	for (int r = 0; r < ch; r++, dst += cw)
		memcpy(dst, pic->v + (size_t)r * (size_t)pic->stride_c, (size_t)cw);
}

/* ---- questions ----------------------------------------------------------------------------------------------- */

const char *tvp9_error(const tvp9 *d)
{
	return d && d->failed ? d->err : NULL;
}

void tvp9_picture_memory(const tvp9 *d, void **base, size_t *size)
{
	if (base)
		*base = d->pic_mem;
	if (size)
		*size = d->pic_bytes;
}

int tvp9_size(const tvp9 *d, int *width, int *height)
{
	if (!d || !d->geo)
		return 0;
	if (width)
		*width = d->w;
	if (height)
		*height = d->h;
	return 1;
}

int tvp9_pictures_decoded(const tvp9 *d)
{
	return d ? d->st_pictures : 0;
}

void tvp9_get_stats(const tvp9 *d, tvp9_stats *s)
{
	const double mhz = tv9p_tick_mhz();

	memset(s, 0, sizeof(*s));
	if (!d)
		return;
	s->frames = d->st_frames;
	s->pictures = d->st_pictures;
	s->dropped = d->st_dropped;
	if (s->frames > 0 && mhz > 0) {
		/* the SPEs count decrementer ticks, the PPU its timebase: the same 79.8 MHz on the PS3 */
		s->entropy_ms = d->st_entropy_ticks / s->frames / mhz / 1000.0;
		s->pixel_ms = d->st_pixel_ticks / s->frames / mhz / 1000.0;
		if (d->st_busy_ticks > 0)
			s->fps = s->frames / (d->st_busy_ticks / mhz / 1e6);
	}
}

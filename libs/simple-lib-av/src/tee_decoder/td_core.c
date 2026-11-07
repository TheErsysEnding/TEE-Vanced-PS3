/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/decoder.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* TEE Decoder: the decoder core on the PPU - see decoder.h.

   (1.3) The input is fed access unit by access unit (dec_feed, any one thread) into an input ring; the
   decoder thread parses them in order. The picture buffers are laid out at a run's first slice, from its SPS.
   Per-picture state lives in rings (PICRING; the statistics in a ring of their own size).

   One decoder thread does all the bookkeeping, so none of it needs a lock: it parses ahead, gives CABAC
   jobs to idle CABAC SPEs (up to cfg.lead pictures ahead of the pixel stage), gives the next picture in
   decode order to an idle pixel SPE as soon as its records are there and a picture buffer is free, and
   handles the answers. The display thread and the hash thread only touch their queues (dec_out and
   hash_entry: state words, written after the rest with a barrier) and wake the decoder thread up.

   Picture buffers (dpb.c) carry holds: the picture's own decoding, every pixel job in flight that refers to
   it, the display (until the display thread gives it back) and the check (until it is hashed). A buffer is
   reused once it has no hold and is no reference. The reference marking (dpb_end) runs when the pixel job
   is given out - it only depends on the slice header - so the next picture's lists are right even while
   this one is still being decoded. */
#include "td_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td_cabac_job.h"
#include "td_h264_hdr.h"
#include "td_pichash.h"
#include "td_plat.h"

#define PICQ          64                /* pictures between parsing and retirement */
#define PICRING       256               /* per-picture flags, slots and time stamps (pictures in flight are far fewer) */
#define PR(i)         ((i) & (PICRING - 1))
#define INQ           256               /* access units fed, not parsed yet */
#define HASHQ         64
#define RBSP_RING     (4u << 20)        /* slice data for the CABAC SPEs */
#define NAL_MAX       (2u << 20)
#define REC_MIN_FREE  (2u << 20)        /* a CABAC job starts with at least this much room in its ring */
#define PARSE_AHEAD   6                 /* parsed pictures waiting for CABAC at most */

enum { P_FREE, P_PARSED, P_CABAC, P_CABAC_DONE, P_PIXEL, P_DONE };

typedef struct {
	int idx, state;
	h264_slice_hdr sh;
	cabac_slice_params cp;
	uint32_t rb_end;                    /* RBSP ring counter after this picture */
	uint32_t rb_data, rb_len;           /* its slice data: ring offset, bytes for the CABAC SPE (padding included) */
	int ring;                           /* records ring = CABAC SPE */
	uint32_t rec_start, rec_bytes, rec_need;
	int rec_freed;
	int slot, nheld;
	int held[2 * PIX_MAX_REFS];
} picture;

typedef struct {
	volatile int32_t state;             /* 0 free, 1 to hash, 2 hashed */
	int32_t pic, slot, pad;
} hash_entry;

typedef struct {
	uint32_t wp, rp;                    /* byte counters */
	int fifo[PICQ];                     /* pictures in allocation order */
	int fh, ft;
} rec_ring_state;

typedef struct {
	uint32_t off, size, end;            /* in the input ring: offset, bytes, the byte counter after it */
	int32_t fmt;                        /* DEC_AU_ANNEXB or the NAL length's bytes */
	int64_t pts;
} in_au;

struct decoder {
	/* the input ring: access units fed by dec_feed (one producer), parsed in order by the decoder thread */
	uint8_t *in;
	uint32_t in_size;
	in_au inq[INQ];
	volatile uint32_t in_tail, in_wp;   /* written by dec_feed */
	volatile uint32_t in_head, in_rp;   /* written by the decoder thread */
	volatile int in_end;                /* dec_feed_end */
	h264_sps sps[32];
	h264_pps pps[256];
	const uint32_t *ref;
	int nref;
	uint8_t *nal;
	/* geometry: from a run's first slice (geom 1); the buffers' last layout (kept when the next run's is the same) */
	int geom, max_w, max_h, slots_req;
	int lay_w, lay_h, lay_refs, lay_slots;
	int width, height, crop_x, crop_y, crop_w, crop_h, reorder, max_refs;
	/* memory */
	uint8_t *rbsp;
	uint8_t *recmem;
	uint32_t rec_total;                 /* bytes of the records block */
	uint32_t rec_ring;                  /* bytes of each CABAC SPE's records ring: rec_total / ncabac */
	uint8_t *dpbmem;
	size_t dpbbytes;
	int dpb_foreign;                    /* dpbmem belongs to the caller */
	uint32_t *prog;                     /* a 128-byte progress line per slot */
	cabac_job *cjob;
	cabac_result *cres;
	pix_job *pjob;
	pix_result *pres;
	dec_picstat *st;                    /* picture k at [k % st_cap] */
	int st_cap;
	int8_t pdone[PICRING];              /* per picture: the pixel stage is done */
	int8_t slot_done[DPB_SLOTS];        /* the picture in this buffer is done (its rows need no waiting) */
	int8_t pslot[PICRING];              /* per picture: its picture buffer */
	int64_t ppts[PICRING];              /* per picture: its access unit's time stamp */
	/* run state */
	dec_cfg cfg;
	dpb dpb;
	picture q[PICQ];
	int q_head, q_tail;                 /* decode indices: live pictures are q_head .. q_tail - 1 */
	int next_pix, rb_free;
	int eos;
	int skipped;                        /* access units before the run's first IDR / I picture, dropped */
	int concealed;                      /* references missing (a run that starts at an I picture), replaced */
	uint32_t rb_wp, rb_rp;
	rec_ring_state rr[DEC_MAX_CABAC];
	int c_busy[DEC_MAX_CABAC], c_pic[DEC_MAX_CABAC];
	int p_busy[PLAT_MAX_SPES], p_pic[PLAT_MAX_SPES];
	uint32_t seq;
	int slot_pic[DPB_SLOTS];
	struct { int pic, poc; } reo[DPB_SLOTS + 4];
	int nreo;
	int out_pic[DEC_OUTQ];
	int next_assign, next_push, next_release;
	volatile int out_total;
	dec_out outq[DEC_OUTQ];
	hash_entry hq[HASHQ];
	int hq_push, hq_release;
	void *dthread, *hthread;
	volatile int stop, done, hung;
	char error[200];
	int pics_done;
	uint64_t t_start, t_end, last_answer;
	dec_spestat spe[PLAT_MAX_SPES];
};

#define Q(d, i) (&(d)->q[(i) % PICQ])
#define ST(d, i) (&(d)->st[(i) % (d)->st_cap])

static void fail(decoder *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void fail(decoder *d, const char *fmt, ...)
{
	va_list ap;

	if (d->error[0])
		return;
	va_start(ap, fmt);
	vsnprintf(d->error, sizeof(d->error), fmt, ap);
	va_end(ap);
	plat_log("decoder: %s", d->error);
	d->stop = 1;
}

/* ---- set-up ------------------------------------------------------------------------------------------------ */

static void *alloc128(size_t n)
{
	return plat_alloc(n, 128);
}

static uint8_t *dpb_arena, *dpb_arena_end;

static void *dpb_alloc(size_t n)
{
	uint8_t *p = dpb_arena;

	if (!p || p + n > dpb_arena_end)
		return NULL;
	dpb_arena += (n + 127) & ~(size_t)127;
	return p;
}

size_t dec_dpb_bytes_for(int max_width, int max_height, int slots)
{
	if (slots <= 0)
		slots = 16;
	if (slots > DPB_SLOTS)
		slots = DPB_SLOTS;
	return ((dpb_slot_bytes(max_width, max_height) * (size_t)slots) + (1u << 20) - 1) & ~(size_t)((1u << 20) - 1);
}

decoder *dec_create(const dec_setup *su, char *err, int errlen)
{
	decoder *d = calloc(1, sizeof(decoder));
	uint32_t rec_ring = su->rec_ring ? su->rec_ring : 32u << 20;

	if (!d) {
		snprintf(err, (size_t)errlen, "no memory for the decoder");
		return NULL;
	}
	d->ref = su->ref;
	d->nref = su->nref;
	d->out_total = -1;
	d->max_w = su->max_width > 0 ? su->max_width : 1920;
	d->max_h = su->max_height > 0 ? su->max_height : 1088;
	if (d->max_w > 1920 || d->max_h > 1088 || d->max_w < 128 || d->max_h < 32) {
		snprintf(err, (size_t)errlen, "maximum picture size %dx%d not supported (128x32 .. 1920x1088)", d->max_w, d->max_h);
		goto bad;
	}
	d->slots_req = su->slots > 0 ? su->slots : 16;
	if (d->slots_req > DPB_SLOTS)
		d->slots_req = DPB_SLOTS;
	if ((rec_ring & (rec_ring - 1)) || rec_ring < (1u << 20)) {
		snprintf(err, (size_t)errlen, "the records ring must be a power of two of 1 MB or more");
		goto bad;
	}
	d->in_size = ((su->in_bytes ? su->in_bytes : 8u << 20) + 127) & ~127u;
	d->st_cap = su->stats_pictures > 256 ? su->stats_pictures : 256;   /* more than the pictures in flight */
	d->nal = malloc(NAL_MAX + 64);
	/* the picture buffers: one block, 1 MB aligned (the RSX maps it in whole megabytes), for the largest size */
	d->dpbbytes = dec_dpb_bytes_for(d->max_w, d->max_h, d->slots_req);
	if (su->dpb_block) {                    /* the caller's block (kept mapped for the RSX across streams) */
		if (su->dpb_bytes < d->dpbbytes) {
			snprintf(err, (size_t)errlen, "picture buffers: %u MB needed, the block has %u MB", (unsigned)(d->dpbbytes >> 20),
			         (unsigned)(su->dpb_bytes >> 20));
			goto bad;
		}
		d->dpbmem = su->dpb_block;
		d->dpbbytes = su->dpb_bytes;
		d->dpb_foreign = 1;
	} else {
		d->dpbmem = plat_alloc(d->dpbbytes, 1u << 20);
	}
	d->rec_total = rec_ring;
	d->rec_ring = rec_ring;
	d->recmem = plat_alloc((size_t)rec_ring, 128);
	d->rbsp = plat_alloc(RBSP_RING, 128);
	d->in = plat_alloc(d->in_size, 128);
	d->prog = plat_alloc(128 * DPB_SLOTS, 128);
	d->cjob = plat_alloc(sizeof(cabac_job) * DEC_MAX_CABAC, 128);
	d->cres = plat_alloc(sizeof(cabac_result) * DEC_MAX_CABAC, 128);
	d->pjob = plat_alloc(sizeof(pix_job) * PLAT_MAX_SPES, 128);
	d->pres = plat_alloc(sizeof(pix_result) * PLAT_MAX_SPES, 128);
	d->st = calloc((size_t)d->st_cap, sizeof(dec_picstat));
	if (!d->nal || !d->dpbmem || !d->recmem || !d->rbsp || !d->in || !d->prog || !d->cjob || !d->cres || !d->pjob || !d->pres || !d->st) {
		snprintf(err, (size_t)errlen, "no memory: %u MB picture buffers (%d), %u MB records, %u MB input", (unsigned)(d->dpbbytes >> 20),
		         d->slots_req, (unsigned)(rec_ring >> 20), (unsigned)(d->in_size >> 20));
		goto bad;
	}
	plat_log("decoder: buffers for up to %dx%d: %d picture buffers (%u MB), records %u MB, input %u MB, statistics for %d pictures",
	         d->max_w, d->max_h, d->slots_req, (unsigned)(d->dpbbytes >> 20), (unsigned)(rec_ring >> 20), (unsigned)(d->in_size >> 20),
	         d->st_cap);
	(void)alloc128;
	return d;
bad:
	dec_destroy(d);
	return NULL;
}

/* the picture buffers laid out for the stream's size at a run's first slice: at least its references + 4
   (as many as fit the block, never more than asked unless the references need them) */
static int geometry(decoder *d, const h264_sps *sps)
{
	int slots = d->slots_req;
	size_t slot_bytes;

	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8 || !sps->frame_mbs_only || sps->mb_width * 16 > d->max_w ||
	    sps->mb_height * 16 > d->max_h || sps->mb_width % 8 || sps->mb_height < 2) {
		fail(d, "stream not supported: %dx%d MBs (buffers for %dx%d), chroma %d, depth %d, frames only %d", sps->mb_width,
		     sps->mb_height, d->max_w, d->max_h, sps->chroma_format_idc, sps->bit_depth_luma, sps->frame_mbs_only);
		return -1;
	}
	d->width = sps->mb_width * 16;
	d->height = sps->mb_height * 16;
	d->crop_x = sps->crop_left * 2;
	d->crop_y = sps->crop_top * 2;
	d->crop_w = d->width - 2 * (sps->crop_left + sps->crop_right);
	d->crop_h = d->height - 2 * (sps->crop_top + sps->crop_bottom);
	d->max_refs = sps->num_ref_frames;
	d->reorder = sps->max_num_reorder_frames >= 0 ? sps->max_num_reorder_frames : sps->num_ref_frames;
	if (slots < d->max_refs + 4)
		slots = d->max_refs + 4;
	if (slots > DPB_SLOTS)
		slots = DPB_SLOTS;
	slot_bytes = dpb_slot_bytes(d->width, d->height);
	while (slots > d->max_refs + 2 && slot_bytes * (size_t)slots > d->dpbbytes)
		slots--;
	if (d->lay_w == d->width && d->lay_h == d->height && d->lay_refs == d->max_refs && d->lay_slots == slots) {
		d->geom = 1;                        /* the same layout as the last run: dec_start reset it */
		return 0;
	}
	d->lay_w = 0;
	dpb_arena = d->dpbmem;
	dpb_arena_end = d->dpbmem + d->dpbbytes;
	if (slot_bytes * (size_t)slots > d->dpbbytes || dpb_init(&d->dpb, d->width, d->height, d->max_refs, slots, dpb_alloc) != 0) {
		fail(d, "picture buffers do not fit: %d of %u KB in %u MB", slots, (unsigned)(slot_bytes >> 10), (unsigned)(d->dpbbytes >> 20));
		return -1;
	}
	d->lay_w = d->width;
	d->lay_h = d->height;
	d->lay_refs = d->max_refs;
	d->lay_slots = slots;
	d->geom = 1;
	plat_log("decoder: %dx%d (crop %dx%d at %d,%d), %d refs, reorder %d, %d picture buffers of %u KB", d->width, d->height,
	         d->crop_w, d->crop_h, d->crop_x, d->crop_y, d->max_refs, d->reorder, slots, (unsigned)(slot_bytes >> 10));
	return 0;
}

void dec_destroy(decoder *d)
{
	if (!d)
		return;
	free(d->nal);
	if (!d->dpb_foreign)
		plat_free(d->dpbmem);
	plat_free(d->recmem);
	plat_free(d->rbsp);
	plat_free(d->in);
	plat_free(d->prog);
	plat_free(d->cjob);
	plat_free(d->cres);
	plat_free(d->pjob);
	plat_free(d->pres);
	free(d->st);
	free(d);
}

void dec_dpb_block(const decoder *d, void **base, size_t *size)
{
	*base = d->dpbmem;
	*size = d->dpbbytes;
}

const recon_pic *dec_slot_pic(const decoder *d, int slot)
{
	return &d->dpb.s[slot].pic;
}

void dec_crop(const decoder *d, int *x, int *y, int *w, int *h)
{
	*x = d->crop_x;
	*y = d->crop_y;
	*w = d->crop_w;
	*h = d->crop_h;
}

int dec_slots(const decoder *d)
{
	return d->dpb.nslots;
}

int dec_geometry(const decoder *d, int *width, int *height)
{
	if (width)
		*width = d->width;
	if (height)
		*height = d->height;
	return d->geom;
}

int dec_stats_cap(const decoder *d)
{
	return d->st_cap;
}

/* ---- the input ------------------------------------------------------------------------------------------------ */

int dec_feed(decoder *d, const uint8_t *au, size_t size, int format, int64_t pts)
{
	uint32_t need = (uint32_t)((size + 127) & ~(size_t)127), wp = d->in_wp, at;
	in_au *e;

	if (format != DEC_AU_ANNEXB && format != 1 && format != 2 && format != 4)
		return -1;
	if (size == 0 || size > NAL_MAX || need > d->in_size)
		return -1;
	if (d->in_tail - d->in_head >= INQ)
		return 0;
	at = wp % d->in_size;
	if (at + need > d->in_size) {           /* never across the ring's end */
		wp += d->in_size - at;
		at = 0;
	}
	if (wp + need - d->in_rp > d->in_size)
		return 0;
	memcpy(d->in + at, au, size);
	e = &d->inq[d->in_tail % INQ];
	e->off = at;
	e->size = (uint32_t)size;
	e->end = wp + need;
	e->fmt = format;
	e->pts = pts;
	d->in_wp = wp + need;
	plat_sync();
	d->in_tail++;
	plat_wake();
	return 1;
}

void dec_feed_end(decoder *d)
{
	plat_sync();
	d->in_end = 1;
	plat_wake();
}

dec_out *dec_outq(decoder *d)
{
	return d->outq;
}

int dec_out_total(const decoder *d)
{
	return d->out_total;
}

int dec_done(const decoder *d)
{
	return d->done;
}

const char *dec_error(const decoder *d)
{
	return d->error[0] ? d->error : NULL;
}

void dec_wake(decoder *d)
{
	(void)d;
	plat_wake();
}

const dec_picstat *dec_stats(const decoder *d, int *n)
{
	*n = d->pics_done;
	return d->st;
}

dec_picstat *dec_stats_rw(decoder *d)
{
	return d->st;
}

const dec_spestat *dec_spe_stats(const decoder *d, int *n)
{
	*n = d->cfg.ncabac + d->cfg.npixel;
	return d->spe;
}

uint64_t dec_run_ticks(const decoder *d)
{
	return d->t_end - d->t_start;
}

int dec_pictures_done(const decoder *d)
{
	return d->pics_done;
}

/* ---- parsing ------------------------------------------------------------------------------------------------ */

/* the NAL units of an access unit, Annex B or length-prefixed: the next one at *pos, NULL at its end */
static const uint8_t *au_next_nal(const uint8_t *au, size_t size, int fmt, size_t *pos, size_t *len)
{
	size_t n = 0;
	int k;

	if (fmt == DEC_AU_ANNEXB)
		return h264_next_nal(au, size, pos, len);
	if (*pos + (size_t)fmt > size)
		return NULL;
	for (k = 0; k < fmt; k++)
		n = n << 8 | au[*pos + (size_t)k];
	*pos += (size_t)fmt;
	if (n > size - *pos)                    /* cut short: what is there */
		n = size - *pos;
	*len = n;
	*pos += n;
	return au + *pos - n;
}

/* the next access unit: its SPS / PPS kept, its slice (one per picture) unescaped into the RBSP ring.
   1 a picture parsed, 0 none now (no input, or no room yet - the access unit stays), -1 consumed without a
   picture (parameter sets only, or dropped before the run's first IDR / I picture) */
static int parse_picture(decoder *d)
{
	const in_au *e;
	const uint8_t *au, *nal;
	size_t pos = 0, len, n;
	picture *p = Q(d, d->q_tail);
	int got = 0;

	if (d->cfg.max_pictures > 0 && d->q_tail >= d->cfg.max_pictures) {
		d->eos = 1;
		return 0;
	}
	if (d->in_head == d->in_tail) {
		if (d->in_end) {
			plat_sync();
			if (d->in_head == d->in_tail)
				d->eos = 1;
		}
		return 0;
	}
	plat_sync();                                /* the access unit as dec_feed wrote it */
	e = &d->inq[d->in_head % INQ];
	au = d->in + e->off;
	while ((nal = au_next_nal(au, e->size, e->fmt, &pos, &len)) != NULL) {
		int type = len ? nal[0] & 31 : 0;

		if (len < 2 || (type != 1 && type != 5 && type != 7 && type != 8))
			continue;
		if (got && (type == 1 || type == 5)) {
			fail(d, "picture %d: more than one slice per picture (not supported)", d->q_tail);
			return 0;
		}
		if (type == 1 || type == 5) {           /* room for its slice data? (escaped length: an upper bound) */
			uint32_t need = (((uint32_t)len + 32 + 15) & ~15u) + 127, wp = d->rb_wp, at = wp % RBSP_RING;

			need &= ~127u;
			if (at + need > RBSP_RING)
				wp += RBSP_RING - at;
			if (wp + need - d->rb_rp > RBSP_RING)
				return 0;                       /* later, when the CABAC stage has taken more */
		}
		n = h264_unescape(nal, len, d->nal);
		memset(d->nal + n, 0, 64);
		if (type == 7) {
			h264_sps sp;

			if (h264_parse_sps(d->nal, n, &sp) == 0 && sp.sps_id < 32)
				d->sps[sp.sps_id] = sp;
		} else if (type == 8) {
			h264_pps pp;

			if (h264_parse_pps(d->nal, n, d->sps, &pp) == 0)
				d->pps[pp.pps_id] = pp;
		} else {
			const h264_sps *sps;
			const h264_pps *pps;
			uint32_t data_len, need, wp, at;
			dec_picstat *s;

			if (h264_parse_slice_hdr(d->nal, n, d->sps, d->pps, &p->sh) != 0) {
				if (!d->geom) {                 /* joined a stream: its parameter sets come with an IDR picture */
					d->skipped++;
					break;
				}
				fail(d, "picture %d: slice header not usable (no SPS / PPS before it?)", d->q_tail);
				return 0;
			}
			if (p->sh.first_mb != 0) {
				fail(d, "picture %d: more than one slice per picture (not supported)", d->q_tail);
				return 0;
			}
			pps = &d->pps[p->sh.pps_id];
			sps = &d->sps[pps->sps_id];
			if (!d->geom && type != 5 && p->sh.slice_type != SLICE_I) {
				d->skipped++;                   /* the run starts at an IDR or I picture */
				break;
			}
			if (!pps->entropy_coding_mode) {
				fail(d, "picture %d: CAVLC (not supported, only CABAC)", d->q_tail);
				return 0;
			}
			if (!d->geom && geometry(d, sps) != 0)
				return 0;
			if (sps->mb_width * 16 != d->width || sps->mb_height * 16 != d->height) {
				fail(d, "picture %d: the size changed to %dx%d (a new run is needed)", d->q_tail, sps->mb_width * 16,
				     sps->mb_height * 16);
				return 0;
			}
			/* the slice data into the RBSP ring: 128-aligned, 32 zero bytes after it, a multiple of 16 */
			data_len = (uint32_t)(n - p->sh.data_byte);
			p->rb_len = (data_len + 32 + 15) & ~15u;
			need = (p->rb_len + 127) & ~127u;
			wp = d->rb_wp;
			at = wp % RBSP_RING;
			if (at + need > RBSP_RING) {        /* not across the ring's end */
				wp += RBSP_RING - at;
				at = 0;
			}
			memcpy(d->rbsp + at, d->nal + p->sh.data_byte, data_len);
			memset(d->rbsp + at + data_len, 0, need - data_len);
			d->rb_wp = wp + need;
			p->rb_end = d->rb_wp;
			p->rb_data = at;
			memset(&p->cp, 0, sizeof(p->cp));
			p->cp.slice_type = p->sh.slice_type;
			p->cp.slice_qp = p->sh.slice_qp;
			p->cp.cabac_init_idc = p->sh.cabac_init_idc;
			p->cp.num_ref_idx_l0 = p->sh.num_ref_idx_l0;
			p->cp.num_ref_idx_l1 = p->sh.num_ref_idx_l1;
			p->cp.transform_8x8_mode = pps->transform_8x8_mode;
			p->cp.direct_8x8_inference = sps->direct_8x8_inference;
			p->cp.mb_width = sps->mb_width;
			p->cp.mb_height = sps->mb_height;
			p->cp.data_bytes = (int32_t)data_len;
			p->cp.stop_bit = (int32_t)h264_stop_bit(d->nal + p->sh.data_byte, data_len);
			p->idx = d->q_tail;
			p->rec_need = 0;
			p->rec_freed = 0;
			p->nheld = 0;
			p->slot = -1;
			p->state = P_PARSED;
			s = ST(d, d->q_tail);
			memset(s, 0, sizeof(*s));
			s->type = (int8_t)(p->sh.slice_type % 3);
			s->cabac_spe = s->pixel_spe = -1;
			s->hash_ok = -1;
			s->out_idx = -1;
			s->vb_shown = s->vb_due = -1;
			s->slice_bytes = data_len;
			s->t_parse = plat_ticks();
			d->pdone[PR(d->q_tail)] = 0;
			d->ppts[PR(d->q_tail)] = e->pts;
			got = 1;
		}
	}
	/* the access unit is used up: its room goes back to dec_feed */
	plat_sync();
	d->in_rp = e->end;
	d->in_head++;
	if (!got)
		return -1;
	d->q_tail++;
	return 1;
}

static int parse_more(decoder *d)
{
	int did = 0;

	while (!d->eos && !d->stop && d->q_tail - d->q_head < PICQ) {
		int waiting = 0, i, r;

		for (i = d->next_pix; i < d->q_tail; i++)
			if (Q(d, i)->state == P_PARSED)
				waiting++;
		if (waiting >= PARSE_AHEAD)
			break;
		r = parse_picture(d);
		if (r == 0)
			break;
		did = 1;
	}
	return did;
}

/* ---- CABAC ---------------------------------------------------------------------------------------------- */

static int issue_cabac(decoder *d)
{
	int c, did = 0;

	for (c = 0; c < d->cfg.ncabac && !d->stop; c++) {
		rec_ring_state *r = &d->rr[c];
		uint32_t start, room, need;
		int i, ahead = 0;
		picture *p = NULL;
		cabac_job *j = &d->cjob[c];

		if (d->c_busy[c])
			continue;
		for (i = d->next_pix; i < d->q_tail; i++) {
			int s = Q(d, i)->state;

			if (s == P_CABAC || s == P_CABAC_DONE)
				ahead++;
			else if (s == P_PARSED && !p)
				p = Q(d, i);
		}
		if (!p || ahead >= d->cfg.lead)
			break;
		start = (r->wp + 127) & ~127u;
		room = d->rec_ring - (start - r->rp);
		need = p->rec_need > REC_MIN_FREE ? p->rec_need : REC_MIN_FREE;
		if (need > d->rec_ring)
			need = d->rec_ring;
		if (room < need)
			continue;                                   /* this ring is full; maybe the other one */
		memset(j, 0, sizeof(*j));
		j->seq = ++d->seq;
		j->data_size = p->rb_len;
		j->out_cap = room;
		j->data_ea = plat_ea(d->rbsp + p->rb_data);
		j->out_ea = plat_ea(d->recmem + (size_t)c * d->rec_ring);
		j->result_ea = plat_ea(&d->cres[c]);
		j->out_off = start & (d->rec_ring - 1);
		j->out_ring = d->rec_ring;
		j->prm = p->cp;
		p->state = P_CABAC;
		p->ring = c;
		p->rec_start = start;
		d->c_busy[c] = 1;
		d->c_pic[c] = p->idx;
		ST(d, p->idx)->cabac_spe = (int8_t)c;
		ST(d, p->idx)->cabac_tries++;
		ST(d, p->idx)->t_cabac_in = plat_ticks();
		plat_sync();
		if (plat_spe_send(c, plat_ea(j)) != 0) {
			fail(d, "the mailbox of CABAC SPE %d refused a job", c);
			return did;
		}
		did = 1;
	}
	return did;
}

static void cabac_answer(decoder *d, int c, uint32_t word)
{
	cabac_result *res = &d->cres[c];
	picture *p;
	dec_picstat *s;

	if (!d->c_busy[c] || (word >> 8) != (d->cjob[c].seq & 0xFFFF)) {
		fail(d, "unexpected answer %06x from CABAC SPE %d", (unsigned)word, c);
		return;
	}
	d->c_busy[c] = 0;
	p = Q(d, d->c_pic[c]);
	s = ST(d, p->idx);
	s->t_cabac_out = plat_ticks();
	s->cabac_ticks = res->ticks_total;
	s->cabac_wait = res->ticks_wait_in + res->ticks_wait_out;
	d->spe[c].jobs++;
	d->spe[c].busy += res->ticks_total;
	d->spe[c].waited += res->ticks_wait_in + res->ticks_wait_out;
	if (res->status == RES_OUT_FULL) {                  /* did not fit: again, once that much room is there */
		p->rec_need = res->out_bytes + 256;
		p->state = P_PARSED;
		plat_log("picture %d: records of %u bytes did not fit into %u, CABAC again later", p->idx, (unsigned)res->out_bytes,
		         (unsigned)d->cjob[c].out_cap);
		return;
	}
	if (res->status != RES_OK || res->st.error || !res->st.end_ok || res->st.mbs != (uint32_t)(p->cp.mb_width * p->cp.mb_height)) {
		fail(d, "picture %d: CABAC status %u error %d end %d after %u MBs", p->idx, (unsigned)res->status, (int)res->st.error,
		     (int)res->st.end_ok, (unsigned)res->st.mbs);
		return;
	}
	p->rec_bytes = res->out_bytes;
	s->rec_bytes = res->out_bytes;
	d->rr[c].wp = p->rec_start + res->out_bytes;
	d->rr[c].fifo[d->rr[c].ft++ % PICQ] = p->idx;
	p->state = P_CABAC_DONE;
}

/* ---- pixel stage -------------------------------------------------------------------------------------- */

static void job_params(recon_params *rp, const h264_sps *sps, const h264_pps *pps, const h264_slice_hdr *sh)
{
	rp->slice_type = sh->slice_type;
	rp->mb_w = sps->mb_width;
	rp->mb_h = sps->mb_height;
	rp->direct_spatial = sh->direct_spatial_mv_pred;
	rp->direct_8x8_inference = sps->direct_8x8_inference;
	rp->transform_8x8_mode = pps->transform_8x8_mode;
	rp->cqp_offset[0] = pps->chroma_qp_index_offset;
	rp->cqp_offset[1] = pps->second_chroma_qp_index_offset;
	rp->disable_deblock = sh->disable_deblocking;
	rp->alpha_offset = sh->alpha_offset;
	rp->beta_offset = sh->beta_offset;
	rp->wp_mode = 0;
	if (sh->slice_type == SLICE_P && pps->weighted_pred)
		rp->wp_mode = 1;
	else if (sh->slice_type == SLICE_B && pps->weighted_bipred_idc == 1)
		rp->wp_mode = 1;
	else if (sh->slice_type == SLICE_B && pps->weighted_bipred_idc == 2)
		rp->wp_mode = 2;
	if (rp->wp_mode == 1) {
		int l, r;

		rp->luma_log2 = sh->luma_log2_denom;
		rp->chroma_log2 = sh->chroma_log2_denom;
		for (l = 0; l < 2; l++)
			for (r = 0; r < RECON_MAX_REFS; r++) {
				rp->lw[l][r] = sh->luma_weight[l][r];
				rp->lo[l][r] = sh->luma_offset[l][r];
				rp->cw[l][r][0] = sh->chroma_weight[l][r][0];
				rp->cw[l][r][1] = sh->chroma_weight[l][r][1];
				rp->co[l][r][0] = sh->chroma_offset[l][r][0];
				rp->co[l][r][1] = sh->chroma_offset[l][r][1];
			}
	}
}

static int slot_of(const decoder *d, const recon_pic *pic)
{
	return (int)(((const uint8_t *)pic - (const uint8_t *)&d->dpb.s[0].pic) / (ptrdiff_t)sizeof(dpb_slot));
}

/* output order: up to `reorder` pictures wait; the one with the smallest POC goes next */
static void try_push(decoder *d);

static void assign_min(decoder *d)
{
	int k, best = 0;

	for (k = 1; k < d->nreo; k++)
		if (d->reo[k].poc < d->reo[best].poc)
			best = k;
	d->out_pic[d->next_assign % DEC_OUTQ] = d->reo[best].pic;
	ST(d, d->reo[best].pic)->out_idx = d->next_assign;
	d->next_assign++;
	d->reo[best] = d->reo[--d->nreo];
}

static int has_mmco5(const h264_slice_hdr *sh)
{
	int k;

	for (k = 0; sh->adaptive_marking && k < sh->mmco_count; k++)
		if (sh->mmco_op[k] == 5)
			return 1;
	return 0;
}

static int issue_pixel(decoder *d)
{
	picture *p;
	const h264_sps *sps;
	const h264_pps *pps;
	static recon_params rp;
	pix_job *j;
	const recon_pic *pic;
	int s, slot, l, i, idr, is_ref;

	if (d->next_pix >= d->q_tail || d->stop)
		return 0;
	p = Q(d, d->next_pix);
	if (p->state != P_CABAC_DONE)
		return 0;
	for (s = 0; s < d->cfg.npixel && d->p_busy[s]; s++)
		;
	if (s == d->cfg.npixel)
		return 0;
	idr = p->sh.nal_unit_type == 5;
	if (dpb_free_slots(&d->dpb, idr) < 1)
		return 0;
	pps = &d->pps[p->sh.pps_id];
	sps = &d->sps[pps->sps_id];
	memset(&rp, 0, sizeof(rp));
	slot = dpb_begin(&d->dpb, sps, pps, &p->sh, &rp);
	if (slot < 0) {
		fail(d, "picture %d: no reference list (dpb_begin)", p->idx);
		return 0;
	}
	for (l = 0; l < 2; l++)                          /* a reference missing (leading pictures of an open GOP, */
		for (i = 0; i < rp.num_ref[l] && i < PIX_MAX_REFS; i++)   /* when a run starts at an I picture): the nearest one */
			if (!rp.ref[l][i]) {
				int l2, i2;

				for (l2 = 0; l2 < 2 && !rp.ref[l][i]; l2++)
					for (i2 = 0; i2 < rp.num_ref[l2] && i2 < PIX_MAX_REFS && !rp.ref[l][i]; i2++)
						rp.ref[l][i] = rp.ref[l2][i2];
				if (rp.ref[l][i] && d->concealed++ < 5)
					plat_log("picture %d: reference %d of list %d missing, replaced", p->idx, i, l);
			}
	job_params(&rp, sps, pps, &p->sh);
	dpb_end(&d->dpb, sps, &p->sh);
	is_ref = p->sh.nal_ref_idc != 0;
	pic = &d->dpb.s[slot].pic;
	p->slot = slot;
	d->slot_pic[slot] = p->idx;
	d->slot_done[slot] = 0;
	d->pslot[PR(p->idx)] = (int8_t)slot;
	ST(d, p->idx)->poc = rp.cur_poc;

	j = &d->pjob[s];
	memset(j, 0, sizeof(*j));
	j->seq = ++d->seq;
	j->flags = (rp.disable_deblock != 1 ? PIX_DEBLOCK : 0) | (is_ref ? PIX_BORDERS | PIX_PROGRESS : 0) | PIX_WAITREFS;
	j->rec_ea = plat_ea(d->recmem + (size_t)p->ring * d->rec_ring);
	j->rec_ring = d->rec_ring;
	j->rec_off = p->rec_start & (d->rec_ring - 1);
	j->rec_bytes = p->rec_bytes;
	j->out_y = plat_ea(pic->y);
	j->out_u = plat_ea(pic->u);
	j->out_v = plat_ea(pic->v);
	j->stride_y = (uint32_t)pic->stride_y;
	j->stride_c = (uint32_t)pic->stride_c;
	j->col_out = plat_ea(pic->col);
	j->col_in = (p->sh.slice_type == SLICE_B && rp.ref[1][0]) ? plat_ea(rp.ref[1][0]->col) : 0;
	j->prog_ea = plat_ea(&d->prog[slot * 32]);
	j->result_ea = plat_ea(&d->pres[s]);
	j->slice_type = rp.slice_type;
	j->mb_w = rp.mb_w;
	j->mb_h = rp.mb_h;
	j->cur_poc = rp.cur_poc;
	j->out_id = pic->id;
	j->num_ref[0] = rp.num_ref[0] < PIX_MAX_REFS ? rp.num_ref[0] : PIX_MAX_REFS;
	j->num_ref[1] = rp.num_ref[1] < PIX_MAX_REFS ? rp.num_ref[1] : PIX_MAX_REFS;
	j->direct_spatial = rp.direct_spatial;
	j->wp_mode = rp.wp_mode;
	j->luma_log2 = rp.luma_log2;
	j->chroma_log2 = rp.chroma_log2;
	j->cqp_offset[0] = rp.cqp_offset[0];
	j->cqp_offset[1] = rp.cqp_offset[1];
	j->disable_deblock = rp.disable_deblock;
	j->alpha_offset = rp.alpha_offset;
	j->beta_offset = rp.beta_offset;
	p->nheld = 0;
	for (l = 0; l < 2; l++)
		for (i = 0; i < PIX_MAX_REFS; i++) {
			const recon_pic *r = i < j->num_ref[l] ? rp.ref[l][i] : NULL;

			if (r) {
				int rs = slot_of(d, r), k;

				j->ref[l][i].y = plat_ea(r->y);
				j->ref[l][i].u = plat_ea(r->u);
				j->ref[l][i].v = plat_ea(r->v);
				j->ref[l][i].poc = r->poc;
				j->ref[l][i].long_term = r->long_term;
				j->ref[l][i].id = r->id;
				j->ref[l][i].prog = d->slot_done[rs] ? 0 : plat_ea(&d->prog[rs * 32]);
				for (k = 0; k < p->nheld && p->held[k] != rs; k++)
					;
				if (k == p->nheld) {
					p->held[p->nheld++] = rs;
					dpb_hold(&d->dpb, rs);
				}
			} else if (i < j->num_ref[l]) {
				fail(d, "picture %d: reference %d of list %d missing", p->idx, i, l);
				return 0;
			}
			j->lw[l][i] = (int16_t)rp.lw[l][i];
			j->lo[l][i] = (int16_t)rp.lo[l][i];
			j->cw[l][i][0] = (int16_t)rp.cw[l][i][0];
			j->cw[l][i][1] = (int16_t)rp.cw[l][i][1];
			j->co[l][i][0] = (int16_t)rp.co[l][i][0];
			j->co[l][i][1] = (int16_t)rp.co[l][i][1];
		}
	d->prog[slot * 32] = 0;                             /* nothing of this picture is there yet */

	/* output order */
	if (idr || has_mmco5(&p->sh))
		while (d->nreo)
			assign_min(d);
	d->reo[d->nreo].pic = p->idx;
	d->reo[d->nreo].poc = rp.cur_poc;
	d->nreo++;
	while (d->nreo > d->reorder)
		assign_min(d);

	p->state = P_PIXEL;
	d->p_busy[s] = 1;
	d->p_pic[s] = p->idx;
	ST(d, p->idx)->pixel_spe = (int8_t)s;
	ST(d, p->idx)->t_pix_in = plat_ticks();
	d->next_pix++;
	plat_sync();
	if (plat_spe_send(d->cfg.ncabac + s, plat_ea(j)) != 0) {
		fail(d, "the mailbox of pixel SPE %d refused a job", s);
		return 0;
	}
	if (d->eos && d->next_pix == d->q_tail && d->out_total < 0) {   /* the last picture: the rest goes out */
		while (d->nreo)
			assign_min(d);
		d->out_total = d->next_assign;
	}
	try_push(d);
	return 1;
}

static void try_push(decoder *d)
{
	while (d->next_push < d->next_assign) {
		int pic = d->out_pic[d->next_push % DEC_OUTQ];
		dec_out *o = &d->outq[d->next_push % DEC_OUTQ];

		if (!d->pdone[PR(pic)] || o->state != 0)
			break;
		o->pic = pic;
		o->slot = d->pslot[PR(pic)];
		o->pts = d->ppts[PR(pic)];
		plat_sync();
		o->state = 1;
		d->next_push++;
	}
}

static void pixel_answer(decoder *d, int s, uint32_t word)
{
	pix_result *res = &d->pres[s];
	picture *p;
	dec_picstat *st;
	int k, c;

	if (!d->p_busy[s] || (word >> 8) != (d->pjob[s].seq & 0xFFFF)) {
		fail(d, "unexpected answer %06x from pixel SPE %d", (unsigned)word, s);
		return;
	}
	d->p_busy[s] = 0;
	p = Q(d, d->p_pic[s]);
	st = ST(d, p->idx);
	st->t_pix_out = plat_ticks();
	st->pix_ticks = res->ticks_total;
	st->pix_rows = res->ticks_rows;
	st->pix_polls = res->ref_polls;
	st->fetch_bytes = res->fetch_bytes;
	for (k = 0; k < PIX_PARTS; k++)
		st->parts[k] = res->parts[k];
	for (k = 0; k < PIX_APROF; k++)
		st->aprof[k] = res->aprof[k];
	d->spe[d->cfg.ncabac + s].jobs++;
	d->spe[d->cfg.ncabac + s].busy += res->ticks_total;
	d->spe[d->cfg.ncabac + s].waited += res->parts[RT_REFWAIT];
	if (res->status != PIX_OK) {
		fail(d, "picture %d: pixel SPE %d status %u error %u after %u MBs", p->idx, s, (unsigned)res->status, (unsigned)res->error,
		     (unsigned)res->mbs);
		return;
	}
	p->state = P_DONE;
	d->pics_done++;
	d->t_end = st->t_pix_out;
	/* holds: the display and the check take theirs before the decoding's and the references' go */
	dpb_hold(&d->dpb, p->slot);
	if (d->cfg.check) {
		hash_entry *e = &d->hq[d->hq_push % HASHQ];

		dpb_hold(&d->dpb, p->slot);
		e->pic = p->idx;
		e->slot = p->slot;
		plat_sync();
		e->state = 1;
		d->hq_push++;
	}
	plat_sync();
	d->pdone[PR(p->idx)] = 1;
	d->slot_done[p->slot] = 1;
	dpb_release(&d->dpb, p->slot);
	for (k = 0; k < p->nheld; k++)
		dpb_release(&d->dpb, p->held[k]);
	p->nheld = 0;
	/* its records: the ring moves on over every finished picture in allocation order */
	for (c = 0; c < d->cfg.ncabac; c++) {
		rec_ring_state *r = &d->rr[c];

		while (r->fh != r->ft) {
			picture *q = Q(d, r->fifo[r->fh % PICQ]);

			if (q->state != P_DONE)
				break;
			r->rp = q->rec_start + q->rec_bytes;
			q->rec_freed = 1;
			r->fh++;
		}
	}
	try_push(d);
}

/* ---- the queues the other threads give back, retirement ----------------------------------------------- */

static int take_back(decoder *d)
{
	int did = 0;

	while (d->next_release < d->next_push) {
		dec_out *o = &d->outq[d->next_release % DEC_OUTQ];

		if (o->state != 2)
			break;
		plat_sync();
		dpb_release(&d->dpb, o->slot);
		o->state = 0;
		d->next_release++;
		did = 1;
	}
	while (d->hq_release < d->hq_push) {
		hash_entry *e = &d->hq[d->hq_release % HASHQ];

		if (e->state != 2)
			break;
		plat_sync();
		dpb_release(&d->dpb, e->slot);
		e->state = 0;
		d->hq_release++;
		did = 1;
	}
	if (did)
		try_push(d);
	/* RBSP data: free in decode order once the CABAC stage has it */
	while (d->rb_free < d->q_tail && Q(d, d->rb_free)->state >= P_CABAC_DONE) {
		d->rb_rp = Q(d, d->rb_free)->rb_end;
		d->rb_free++;
		did = 1;
	}
	while (d->q_head < d->q_tail && d->q_head < d->rb_free) {
		picture *p = Q(d, d->q_head);

		if (p->state != P_DONE || !p->rec_freed)
			break;
		p->state = P_FREE;
		d->q_head++;
		did = 1;
	}
	return did;
}

static int all_done(const decoder *d)
{
	int k;

	if (!d->eos || d->q_head != d->q_tail || d->out_total < 0 || d->next_release != d->out_total || d->hq_release != d->hq_push)
		return 0;
	for (k = 0; k < d->cfg.ncabac; k++)
		if (d->c_busy[k])
			return 0;
	for (k = 0; k < d->cfg.npixel; k++)
		if (d->p_busy[k])
			return 0;
	return 1;
}

static int any_busy(const decoder *d)
{
	int k;

	for (k = 0; k < d->cfg.ncabac; k++)
		if (d->c_busy[k])
			return 1;
	for (k = 0; k < d->cfg.npixel; k++)
		if (d->p_busy[k])
			return 1;
	return 0;
}

static void decoder_thread(void *arg)
{
	decoder *d = arg;
	const uint64_t timeout = (uint64_t)(d->cfg.timeout_ms * 1000.0 * plat_tick_mhz());

	d->last_answer = plat_ticks();
	while (!d->stop) {
		int did = take_back(d), spe, r;
		uint32_t word, value;

		did |= parse_more(d);
		did |= issue_cabac(d);
		did |= issue_pixel(d);
		if (all_done(d))
			break;
		if (d->eos && d->out_total < 0 && d->next_pix == d->q_tail) {   /* the stream ended before the pixel stage */
			while (d->nreo)
				assign_min(d);
			d->out_total = d->next_assign;
			try_push(d);
			did = 1;
		}
		r = plat_wait(did ? 1 : 2000, &spe, &word, &value);
		if (r < 0) {
			fail(d, "waiting for the SPEs failed");
			break;
		}
		if (r == 1) {
			d->last_answer = plat_ticks();
			if (spe < d->cfg.ncabac)
				cabac_answer(d, spe, word);
			else
				pixel_answer(d, spe - d->cfg.ncabac, word);
		} else if (any_busy(d) && plat_ticks() - d->last_answer > timeout) {
			d->hung = 1;
			fail(d, "no SPE answered for %d ms (%d pictures done)", d->cfg.timeout_ms, d->pics_done);
			break;
		}
	}
	d->done = 1;
}

/* ---- the check: every picture hashed on the PPU and compared with the reference --------------------------- */

static void hash_thread(void *arg)
{
	decoder *d = arg;
	int at = 0;

	while (!d->stop) {
		hash_entry *e = &d->hq[at % HASHQ];

		if (e->state == 1) {
			const recon_pic *pic = &d->dpb.s[e->slot].pic;
			dec_picstat *s = ST(d, e->pic);
			uint32_t h;

			plat_sync();
			h = pichash_picture(pic->y + d->crop_y * pic->stride_y + d->crop_x, pic->stride_y,
			                    pic->u + (d->crop_y / 2) * pic->stride_c + d->crop_x / 2,
			                    pic->v + (d->crop_y / 2) * pic->stride_c + d->crop_x / 2, pic->stride_c, d->crop_w, d->crop_h);
			s->hash = h;
			s->hash_ok = (int8_t)(d->ref && e->pic < d->nref ? h == d->ref[e->pic] : -1);
			s->t_hash = plat_ticks();
			plat_sync();
			e->state = 2;
			at++;
			plat_wake();
		} else if (d->done) {
			break;
		} else {
			plat_sleep_us(300);
		}
	}
}

/* ---- a run ---------------------------------------------------------------------------------------------------- */

int dec_start(decoder *d, const dec_cfg *cfg)
{
	int k;

	if (cfg->ncabac < 1 || cfg->ncabac > DEC_MAX_CABAC || cfg->npixel < 1 || cfg->ncabac + cfg->npixel > PLAT_MAX_SPES) {
		snprintf(d->error, sizeof(d->error), "bad SPE counts %d + %d", cfg->ncabac, cfg->npixel);
		return -1;
	}
	d->cfg = *cfg;
	d->rec_ring = d->rec_total / (uint32_t)cfg->ncabac;    /* still a power of two (1 or 2 CABAC SPEs) */
	if (d->cfg.lead < 1)
		d->cfg.lead = 1;
	if (d->cfg.timeout_ms < 100)
		d->cfg.timeout_ms = 5000;
	d->geom = 0;                                         /* laid out again at the first slice */
	d->eos = 0;
	d->skipped = d->concealed = 0;
	d->q_head = d->q_tail = d->next_pix = d->rb_free = 0;
	d->rb_wp = d->rb_rp = 0;
	memset(d->q, 0, sizeof(d->q));
	memset(d->rr, 0, sizeof(d->rr));
	memset(d->c_busy, 0, sizeof(d->c_busy));
	memset(d->p_busy, 0, sizeof(d->p_busy));
	memset(d->slot_pic, 0, sizeof(d->slot_pic));
	d->nreo = 0;
	d->next_assign = d->next_push = d->next_release = 0;
	d->out_total = -1;
	memset(d->outq, 0, sizeof(d->outq));
	memset(d->hq, 0, sizeof(d->hq));
	d->hq_push = d->hq_release = 0;
	d->stop = d->done = d->hung = 0;
	d->error[0] = 0;
	d->pics_done = 0;
	memset(d->spe, 0, sizeof(d->spe));
	for (k = 0; k < d->cfg.ncabac + d->cfg.npixel; k++)
		d->spe[k].kind = k < d->cfg.ncabac ? 0 : 1;
	memset(d->st, 0, sizeof(dec_picstat) * (size_t)d->st_cap);
	memset(d->pdone, 0, sizeof(d->pdone));
	memset(d->slot_done, 0, sizeof(d->slot_done));
	for (k = 0; k < DPB_SLOTS; k++)
		d->prog[k * 32] = 0;
	dpb_reset(&d->dpb);
	plat_sync();
	if (plat_spes_start(d->cfg.ncabac, d->cfg.npixel) != 0) {
		snprintf(d->error, sizeof(d->error), "the SPEs did not start (%d CABAC + %d pixel)", d->cfg.ncabac, d->cfg.npixel);
		return -1;
	}
	d->t_start = d->t_end = plat_ticks();
	d->hthread = d->cfg.check ? plat_thread_start(hash_thread, d, 1800, "teedhash") : NULL;
	d->dthread = plat_thread_start(decoder_thread, d, 600, "teedec");
	if (!d->dthread || (d->cfg.check && !d->hthread)) {
		d->stop = 1;
		if (d->hthread)
			plat_thread_join(d->hthread);
		plat_spes_stop(0);
		snprintf(d->error, sizeof(d->error), "the decoder threads did not start");
		return -1;
	}
	return 0;
}

void dec_stop(decoder *d)
{
	d->stop = 1;
	plat_wake();
	if (d->dthread)
		plat_thread_join(d->dthread);
	if (d->hthread)
		plat_thread_join(d->hthread);
	d->dthread = d->hthread = NULL;
	plat_spes_stop(d->hung);
	if (d->skipped || d->concealed)
		plat_log("decoder: %d access units before the first IDR / I picture dropped, %d missing references replaced", d->skipped,
		         d->concealed);
	/* access units not decoded are dropped: the next run starts with an empty input */
	d->in_head = d->in_tail = d->in_wp = d->in_rp = 0;
	d->in_end = 0;
}

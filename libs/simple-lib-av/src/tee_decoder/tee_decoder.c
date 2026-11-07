/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/tee_decoder.c; do not edit here: change the
   project's file and run the script again. */
#include "td_names.h"
/* TEE Decoder - the building block's interface (tee_decoder.h) on top of the decoder core (decoder.c) and the
   platform (plat_ps3.c; mac/plat_sim.c in the Mac simulation). */
#include "tee_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td_core.h"
#include "td_h264_hdr.h"
#include "td_plat.h"

struct tdec {
	decoder *core;
	dec_cfg cfg;
	int running;
	int next_get;                       /* display index of the next picture handed out */
	int gen;                            /* flushes so far: a picture from before one is not released again */
	uint8_t *block;                     /* the picture buffers' memory */
	size_t block_bytes;
	char error[200];
};

void tdec_default(tdec_config *c)
{
	memset(c, 0, sizeof(*c));
	c->tier = TDEC_TIER_1;
	c->max_width = 1920;
	c->max_height = 1088;
	c->pictures = 16;
	c->input_bytes = 8u << 20;
}

static void complete(tdec_config *c)
{
	if (c->tier != TDEC_TIER_2)
		c->tier = TDEC_TIER_1;
	if (c->max_width <= 0)
		c->max_width = 1920;
	if (c->max_height <= 0)
		c->max_height = 1088;
	if (c->pictures <= 0)
		c->pictures = 16;
	if (!c->input_bytes)
		c->input_bytes = 8u << 20;
	if (!c->records_bytes)
		c->records_bytes = 32u << 20;
	if (c->ncabac <= 0 || c->npixel <= 0) {
		c->ncabac = c->tier == TDEC_TIER_2 ? 2 : 1;
		c->npixel = c->tier == TDEC_TIER_2 ? 4 : 3;
	}
	if (c->lead <= 0)
		c->lead = 24;
	if (c->timeout_ms <= 0)
		c->timeout_ms = 5000;
}

size_t tdec_picture_memory_size(const tdec_config *c0)
{
	tdec_config c = *c0;

	complete(&c);
	return dec_dpb_bytes_for(c.max_width, c.max_height, c.pictures);
}

tdec *tdec_open(const tdec_config *c0, char *err, int errlen)
{
	tdec_config c = *c0;
	tdec *d;
	dec_setup su;

	complete(&c);
	plat_set_log(c.log);
	d = calloc(1, sizeof(*d));
	if (!d) {
		snprintf(err, (size_t)errlen, "no memory");
		return NULL;
	}
	if (plat_init() != 0) {
		snprintf(err, (size_t)errlen, "the SPU programs did not load");
		free(d);
		return NULL;
	}
	memset(&su, 0, sizeof(su));
	su.slots = c.pictures;
	su.rec_ring = c.records_bytes;
	su.in_bytes = c.input_bytes;
	su.max_width = c.max_width;
	su.max_height = c.max_height;
	su.dpb_block = c.picture_memory;
	su.dpb_bytes = c.picture_memory_bytes;
	su.ref = c.check_ref;
	su.nref = c.check_n;
	su.stats_pictures = c.stats_pictures;
	d->core = dec_create(&su, err, errlen);
	if (!d->core) {
		plat_shutdown();
		free(d);
		return NULL;
	}
	{
		void *base;

		dec_dpb_block(d->core, &base, &d->block_bytes);
		d->block = base;
	}
	d->cfg.ncabac = c.ncabac;
	d->cfg.npixel = c.npixel;
	d->cfg.lead = c.lead;
	d->cfg.check = c.check || c.check_ref;
	d->cfg.max_pictures = c.max_pictures;
	d->cfg.timeout_ms = c.timeout_ms;
	if (dec_start(d->core, &d->cfg) != 0) {
		snprintf(err, (size_t)errlen, "%s", dec_error(d->core) ? dec_error(d->core) : "the decoder did not start");
		dec_destroy(d->core);
		plat_shutdown();
		free(d);
		return NULL;
	}
	d->running = 1;
	plat_log("TEE Decoder %s: %d CABAC + %d pixel SPEs, up to %dx%d", TEE_DECODER_VERSION, c.ncabac, c.npixel, c.max_width,
	         c.max_height);
	return d;
}

void tdec_close(tdec *d)
{
	if (!d)
		return;
	if (d->running)
		dec_stop(d->core);
	dec_destroy(d->core);
	plat_shutdown();
	free(d);
}

int tdec_feed(tdec *d, const void *au, size_t size, int format, int64_t pts)
{
	return dec_feed(d->core, au, size, format, pts);
}

int tdec_feed_avcc_config(tdec *d, const void *avcc, size_t size)
{
	const uint8_t *a = avcc;
	uint8_t *buf;
	size_t pos = 6, o = 0;
	int lsize, list, k, r;

	if (!a || size < 7 || a[0] != 1 || (a[4] & 3) == 2)
		return -1;
	lsize = (a[4] & 3) + 1;
	buf = malloc(size + 2 * (31 + 255) + 16);       /* every 2-byte length becomes a 4-byte one */
	if (!buf)
		return -1;
	for (list = 0; list < 2; list++) {              /* the SPSs, then the PPSs */
		int n;

		if (list == 0) {
			n = a[5] & 31;
		} else {
			if (pos >= size)
				break;
			n = a[pos++];
		}
		for (k = 0; k < n; k++) {
			size_t len;

			if (pos + 2 > size)
				goto bad;
			len = (size_t)a[pos] << 8 | a[pos + 1];
			pos += 2;
			if (len == 0 || pos + len > size)
				goto bad;
			buf[o++] = (uint8_t)(len >> 24);
			buf[o++] = (uint8_t)(len >> 16);
			buf[o++] = (uint8_t)(len >> 8);
			buf[o++] = (uint8_t)len;
			memcpy(buf + o, a + pos, len);
			o += len;
			pos += len;
		}
	}
	if (!o)
		goto bad;
	r = dec_feed(d->core, buf, o, TDEC_AVCC4, -1);
	free(buf);
	return r == 1 ? lsize : r;
bad:
	free(buf);
	return -1;
}

void tdec_end_of_stream(tdec *d)
{
	dec_feed_end(d->core);
}

int tdec_get_picture(tdec *d, tdec_picture *pic)
{
	dec_out *o;
	int total;

	if (!d->running)
		return -1;
	o = &dec_outq(d->core)[d->next_get % DEC_OUTQ];
	if (o->state == 1) {
		const recon_pic *rp;
		int cx, cy, cw, ch;

		plat_sync();                                /* the entry as the decoder thread wrote it */
		rp = dec_slot_pic(d->core, o->slot);
		dec_crop(d->core, &cx, &cy, &cw, &ch);
		pic->y = rp->y + (size_t)cy * (size_t)rp->stride_y + (size_t)cx;
		pic->u = rp->u + (size_t)(cy / 2) * (size_t)rp->stride_c + (size_t)(cx / 2);
		pic->v = rp->v + (size_t)(cy / 2) * (size_t)rp->stride_c + (size_t)(cx / 2);
		pic->stride_y = rp->stride_y;
		pic->stride_c = rp->stride_c;
		pic->width = cw;
		pic->height = ch;
		pic->pts = o->pts;
		pic->index = d->next_get;
		pic->decode_index = o->pic;
		pic->offset_y = (uint32_t)(pic->y - d->block);
		pic->offset_u = (uint32_t)(pic->u - d->block);
		pic->offset_v = (uint32_t)(pic->v - d->block);
		pic->out_ = d->next_get;
		pic->gen_ = d->gen;
		d->next_get++;
		return 1;
	}
	total = dec_out_total(d->core);
	if (total >= 0 && d->next_get >= total)
		return -1;
	if (dec_error(d->core))
		return -1;
	return 0;
}

void tdec_release_picture(tdec *d, const tdec_picture *pic)
{
	dec_out *o;

	if (!d->running || pic->gen_ != d->gen || pic->out_ < 0 || pic->out_ >= d->next_get)
		return;
	o = &dec_outq(d->core)[pic->out_ % DEC_OUTQ];
	if (o->state != 1)
		return;                                     /* released already */
	plat_sync();                                    /* every read of the picture before the decoder may reuse it */
	o->state = 2;
	dec_wake(d->core);
}

void tdec_copy_i420(const tdec_picture *pic, uint8_t *dst)
{
	int r, cw = pic->width / 2, ch = pic->height / 2;

	for (r = 0; r < pic->height; r++)
		memcpy(dst + (size_t)r * (size_t)pic->width, pic->y + (size_t)r * (size_t)pic->stride_y, (size_t)pic->width);
	dst += (size_t)pic->width * (size_t)pic->height;
	for (r = 0; r < ch; r++)
		memcpy(dst + (size_t)r * (size_t)cw, pic->u + (size_t)r * (size_t)pic->stride_c, (size_t)cw);
	dst += (size_t)cw * (size_t)ch;
	for (r = 0; r < ch; r++)
		memcpy(dst + (size_t)r * (size_t)cw, pic->v + (size_t)r * (size_t)pic->stride_c, (size_t)cw);
}

int tdec_flush(tdec *d)
{
	if (d->running)
		dec_stop(d->core);
	d->running = 0;
	d->gen++;
	d->next_get = 0;
	if (dec_start(d->core, &d->cfg) != 0) {
		snprintf(d->error, sizeof(d->error), "%s", dec_error(d->core) ? dec_error(d->core) : "the decoder did not restart");
		return -1;
	}
	d->error[0] = 0;
	d->running = 1;
	return 0;
}

const char *tdec_error(const tdec *d)
{
	if (d->error[0])
		return d->error;
	return dec_error(d->core);
}

void tdec_picture_memory(const tdec *d, void **base, size_t *size)
{
	*base = d->block;
	*size = d->block_bytes;
}

int tdec_size(const tdec *d, int *width, int *height)
{
	int cx, cy, cw, ch;

	if (!dec_geometry(d->core, NULL, NULL)) {
		if (width)
			*width = 0;
		if (height)
			*height = 0;
		return 0;
	}
	dec_crop(d->core, &cx, &cy, &cw, &ch);
	if (width)
		*width = cw;
	if (height)
		*height = ch;
	return 1;
}

int tdec_pictures_decoded(const tdec *d)
{
	return dec_pictures_done(d->core);
}

int tdec_next_au(const uint8_t *data, size_t size, size_t *pos, const uint8_t **au, size_t *len)
{
	return h264_next_au(data, size, pos, au, len);
}

void *tdec_core(tdec *d)
{
	return d->core;
}

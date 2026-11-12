/* TEE Opus - see tee_opus.h. A thin layer over libopus's decoder: the OpusHead, pre-skip, mono to stereo. */
#include "tee_opus.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

struct topus {
	OpusDecoder *dec;
	int channels;                       /* the stream's: 1 or 2 */
	int preskip;                        /* frames to drop at the stream's start */
	int drop;                           /* frames still to drop (pre-skip, or a seek's run-in) */
	float clip[2];                      /* libopus's soft clipping: what it remembers per channel */
	float mono[TOPUS_MAX_FRAMES];       /* a mono stream's packet, before it goes onto both channels */
};

/* whether any of n floats lies beyond -1 .. 1 (or is no number): their bit patterns compared as integers - the PPU is
   slow at comparing floats one by one, and libopus's soft clipping does that three times a sample */
typedef uint32_t __attribute__((may_alias)) bits32;

static int beyond_full_scale(const float *x, int n)
{
	const bits32 *b = (const bits32 *)x;
	uint32_t over = 0;
	int i;

	for (i = 0; i < n; i++)
		over |= 0x3F800000u - (b[i] & 0x7FFFFFFFu);    /* (negative when |x| > 1.0f) */
	return (int)(over >> 31);
}

static topus *open_fail(topus *d, char *err, int errlen, const char *why)
{
	if (err && errlen > 0)
		snprintf(err, (size_t)errlen, "%s", why);
	if (d) {
		if (d->dec)
			opus_decoder_destroy(d->dec);
		free(d);
	}
	return NULL;
}

topus *topus_open(const void *opus_head, size_t size, char *err, int errlen)
{
	const uint8_t *h = opus_head;
	char why[120];
	topus *d;
	int gain, e = 0;

	if (err && errlen > 0)
		err[0] = 0;
	if (!h || size < 19 || memcmp(h, "OpusHead", 8))
		return open_fail(NULL, err, errlen, "not an OpusHead (the audio track's CodecPrivate)");
	if (h[8] & 0xF0) {                  /* (versions 0 .. 15 are compatible with version 1) */
		snprintf(why, sizeof(why), "OpusHead version %d is not known", h[8]);
		return open_fail(NULL, err, errlen, why);
	}
	if (h[18] != 0 || h[9] < 1 || h[9] > 2) {
		snprintf(why, sizeof(why), "%d channels with channel mapping %d: only mono and stereo (mapping 0) are decoded",
		         h[9], h[18]);
		return open_fail(NULL, err, errlen, why);
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return open_fail(NULL, err, errlen, "no memory");
	d->channels = h[9];
	d->preskip = h[10] | h[11] << 8;
	d->drop = d->preskip;
	d->dec = opus_decoder_create(TOPUS_RATE, d->channels, &e);
	if (!d->dec || e != OPUS_OK)
		return open_fail(d, err, errlen, "libopus did not make a decoder (no memory)");
	gain = (int16_t)(h[16] | h[17] << 8);   /* the stream's output gain, dB in Q8 */
	if (gain)
		opus_decoder_ctl(d->dec, OPUS_SET_GAIN(gain));
	return d;
}

void topus_close(topus *d)
{
	if (!d)
		return;
	opus_decoder_destroy(d->dec);
	free(d);
}

/* n frames decoded into `out` (stereo stream) or d->mono (mono stream): what is still to drop goes, the rest is
   stereo in `out` from its beginning */
static int hand_out(topus *d, int n, float *out)
{
	const int skip = d->drop < n ? d->drop : n, left = n - skip;
	int i;

	/* a decoded signal can go beyond full scale where the original was loud: bent back into -1 .. 1 the way libopus
	   does it for its 16-bit output, not cut off. Only for packets that need it (or that follow one that did) */
	{
		float *const x = d->channels == 2 ? out : d->mono;

		if (d->clip[0] != 0 || d->clip[1] != 0 || beyond_full_scale(x, n * d->channels))
			opus_pcm_soft_clip(x, n, d->channels, d->clip);
	}
	d->drop -= skip;
	if (d->channels == 2) {
		if (skip && left)
			memmove(out, out + 2 * skip, (size_t)left * 2 * sizeof(float));
	} else
		for (i = 0; i < left; i++)
			out[2 * i] = out[2 * i + 1] = d->mono[skip + i];
	return left;
}

int topus_decode(topus *d, const void *packet, size_t size, float *stereo_out)
{
	int n;

	if (!d || !packet || size == 0 || size > 0x7FFFFFFF || !stereo_out)
		return -1;
	n = opus_decode_float(d->dec, packet, (opus_int32)size, d->channels == 2 ? stereo_out : d->mono, TOPUS_MAX_FRAMES, 0);
	if (n < 0)
		return -1;
	return hand_out(d, n, stereo_out);
}

int topus_conceal(topus *d, int frames, float *stereo_out)
{
	int n;

	if (!d || !stereo_out || frames < 120 || frames > TOPUS_MAX_FRAMES || frames % 120)
		return -1;
	n = opus_decode_float(d->dec, NULL, 0, d->channels == 2 ? stereo_out : d->mono, frames, 0);
	if (n < 0)
		return -1;
	return hand_out(d, n, stereo_out);
}

int topus_packet_frames(const void *packet, size_t size)
{
	int n;

	if (!packet || size == 0 || size > 0x7FFFFFFF)
		return -1;
	n = opus_packet_get_nb_samples(packet, (opus_int32)size, TOPUS_RATE);
	return n < 0 ? -1 : n;
}

void topus_reset(topus *d, int drop_frames)
{
	if (!d)
		return;
	opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
	d->clip[0] = d->clip[1] = 0;
	d->drop = drop_frames > 0 ? drop_frames : 0;
}

int topus_channels(const topus *d)
{
	return d ? d->channels : 0;
}

int topus_preskip(const topus *d)
{
	return d ? d->preskip : 0;
}

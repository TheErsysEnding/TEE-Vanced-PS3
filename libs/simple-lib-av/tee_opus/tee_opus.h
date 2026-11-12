/* TEE Opus - Opus sound for a player on the PS3, decoded on the PPU with libopus (BSD). A building block beside
   tee_vp9: VP9 at 1440p60 takes all 6 SPEs, so the sound cannot run on an SPE as cellAdec's AAC does. Measured on
   the console beside the full VP9 decoder: 2.4 - 3.5 % of one PPU thread for 160 kbit/s stereo.

   What it decodes: Opus as YouTube serves it in WebM (itag 251, 250, 249: 48 kHz, stereo or mono, channel mapping
   family 0). Not: more than two channels (mapping family 1 and up) - topus_open says so.

   What it gives: interleaved stereo float at 48 kHz, always (a mono stream on both channels), within -1 .. 1 (where
   the decoded signal goes beyond full scale it is bent back softly, as libopus does for 16-bit output) - the form
   the app's decode-aac hands on.

   Use:
     topus *d = topus_open(codec_private, size, err, sizeof(err));    the audio track's CodecPrivate ("OpusHead")
     for every block of the audio track (a WebM SimpleBlock's or Block's payload = one Opus packet):
       float pcm[TOPUS_MAX_FRAMES * 2];
       int n = topus_decode(d, data, size, pcm);       n frames (a frame = one sample for each of the 2 channels)
     seek:  topus_reset(d, TOPUS_SEEK_PREROLL), then feed from 80 ms before the target (WebM's SeekPreRoll)
     end:   topus_close(d)

   Time: the stream starts with "pre-skip" samples that are not sound (the decoder running in); topus_decode drops
   them itself, so the first packets give fewer frames or none. WebM stores the same amount as the track's
   CodecDelay: a packet's first frame that comes out plays at (the block's time stamp - CodecDelay), never before 0.
   After topus_reset(d, n) the first n frames are dropped the same way.

   Calls are synchronous: about half a millisecond for a 20 ms packet of 160 kbit/s stereo on the console, 1.3 ms at
   most. One thread at a time per decoder. libopus is built with its work arrays on the stack: the calls went 21 KB
   deep on the console - give the thread 64 KB. Memory: about 50 KB a decoder, from malloc in topus_open. */
#ifndef TEE_OPUS_H
#define TEE_OPUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEE_OPUS_VERSION    "0.1.0"
#define TOPUS_RATE          48000
#define TOPUS_MAX_FRAMES    5760            /* the most a packet holds: 120 ms */
#define TOPUS_SEEK_PREROLL  3840            /* 80 ms: what the decoder needs to run in after a seek */

typedef struct topus topus;

/* a decoder for the stream with this OpusHead (WebM: the audio track's CodecPrivate, 19 bytes or more; Ogg: the
   first packet). NULL: err says why */
topus *topus_open(const void *opus_head, size_t size, char *err, int errlen);
void   topus_close(topus *d);

/* one packet -> stereo_out (room for TOPUS_MAX_FRAMES * 2 floats). Returns the frames written - 0 while pre-skip or
   a seek's run-in is being dropped - or -1 for a packet that is not Opus (the decoder stays usable: go on with the
   next packet, or cover the gap with topus_conceal) */
int    topus_decode(topus *d, const void *packet, size_t size, float *stereo_out);

/* a packet is missing: `frames` frames (120 .. TOPUS_MAX_FRAMES, a multiple of 120 = 2.5 ms; a YouTube packet is 960)
   made up from what came before, so that the sound does not click. Returns the frames written or -1 */
int    topus_conceal(topus *d, int frames, float *stereo_out);

/* the frames a packet holds, without decoding it (for time stamps); -1: not an Opus packet */
int    topus_packet_frames(const void *packet, size_t size);

/* a seek: the decoder forgets what it heard; the next drop_frames frames it decodes are not handed out
   (TOPUS_SEEK_PREROLL after a seek into the stream, topus_preskip(d) for a restart at the stream's beginning) */
void   topus_reset(topus *d, int drop_frames);

int    topus_channels(const topus *d);     /* the stream's: 1 or 2 (the output is stereo either way) */
int    topus_preskip(const topus *d);      /* frames dropped at the stream's start (WebM's CodecDelay as 48 kHz samples) */

#ifdef __cplusplus
}
#endif

#endif

/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/tee_decoder.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder - an H.264 decoder of its own on the PS3's SPEs, as a building block for apps.

   What it decodes: H.264 High Profile (and Main), 8 bit 4:2:0, CABAC, progressive frames, one slice per
   picture, up to 1920 x 1088. That is what YouTube serves up to 1080p60 and what x264 makes by default.
   Not: CAVLC (Constrained Baseline), interlaced pictures, several slices per picture, 10 bit, 4:2:2. Such a
   stream stops with an error (tdec_error says which), it never shows garbage.

   How fast (measured on the console, 1080p60, every picture equal to ffmpeg's):
     TDEC_TIER_1  1 CABAC + 3 pixel SPEs (4 SPEs, 2 stay free for the app): 60 fps up to 16 Mbit/s
     TDEC_TIER_2  2 CABAC + 4 pixel SPEs (all 6 SPEs): 60 fps beyond 20 Mbit/s
   YouTube's 1080p60 H.264 is 3-12 Mbit/s.

   Use - one decoder at a time:

     tdec_config c;
     tdec_default(&c);                                   tier 1, up to 1920 x 1088
     tdec *d = tdec_open(&c, err, sizeof(err));          the SPEs start (NULL: err says why)

     feeding thread:
       tdec_feed_avcc_config(d, avcc, avcc_size);        MP4 / MKV only: the SPS and PPS from the avcC box
       for every access unit (one picture's NAL units, in decode order):
         while ((r = tdec_feed(d, au, size, format, pts)) == 0)
           sleep a millisecond;                          the input is full: the decoder is ahead
       tdec_end_of_stream(d);                            the rest comes out, then tdec_get_picture says -1

     display thread (60 times a second or whenever it wants one):
       if (tdec_get_picture(d, &pic) == 1) {             the next picture in display order
         draw pic.y / pic.u / pic.v ...                  its planes stay valid until it is released
         tdec_release_picture(d, &pic);                  when the screen (the RSX) is done with it
       }

     seek:  stop feeding, release every picture, tdec_flush(d), then feed from a key frame on
     end:   tdec_close(d)                                the SPEs stop

   Threads: tdec_feed and tdec_end_of_stream from one thread at a time; tdec_get_picture and
   tdec_release_picture from one thread at a time (the same or another). tdec_flush and tdec_close only
   while nothing is being fed. The decoder runs its own PPU thread (and the SPEs) in between.

   Pictures: the decoder writes them straight into its picture buffers (one memory block, see
   tdec_picture_memory); an app can draw them from there without a copy - map the block for the RSX once
   and use pic.offset_y / _u / _v - or copy them out (tdec_copy_i420). Hold at most 3 at a time: a held
   picture keeps its buffer, and with too few free buffers the decoder waits.

   Access units: what a demuxer gives as one sample (MP4, MKV) or what h264_next_au cuts from an Annex B
   stream (.264, MPEG-TS). A stream may start anywhere: pictures before its first IDR or I picture are
   dropped (the parameter sets must be there by then - in the stream, or from tdec_feed_avcc_config). */
#ifndef TEE_DECODER_H
#define TEE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEE_DECODER_VERSION "1.3.0"

#define TDEC_TIER_1 1               /* 1 CABAC + 3 pixel SPEs: 4 SPEs */
#define TDEC_TIER_2 2               /* 2 CABAC + 4 pixel SPEs: 6 SPEs */

/* access unit formats for tdec_feed */
#define TDEC_ANNEXB 0               /* start codes 00 00 01 (.264, MPEG-TS) */
#define TDEC_AVCC4  4               /* NAL units each with a 4-byte big-endian length (MP4, MKV); 1 and 2 work
                                       too: avcC's lengthSizeMinusOne + 1, which tdec_feed_avcc_config returns */

typedef struct tdec tdec;

typedef struct {
	int tier;                       /* TDEC_TIER_1 (default) or TDEC_TIER_2 */
	int max_width, max_height;      /* the largest stream it is set up for (default 1920 x 1088) */
	int pictures;                   /* picture buffers (default 16 = 65 MB at 1080p; raised to the stream's
	                                   reference pictures + 4 when the memory block has room) */
	uint32_t input_bytes;           /* access units fed and not parsed yet (default 8 MB) */
	void *picture_memory;           /* optional: the picture buffers' block, 1 MB aligned and at least
	                                   tdec_picture_memory_size(&config) bytes (e.g. one the app has mapped for
	                                   the RSX); NULL: the decoder allocates it (tdec_picture_memory says where) */
	size_t picture_memory_bytes;
	void (*log)(const char *line);  /* optional: the decoder's log lines (any thread) */

	/* for tests (the TEE Decoder test app, mac/tdec_sim); 0 = the default */
	int ncabac, npixel;             /* SPEs instead of the tier's */
	int lead;                       /* pictures the CABAC stage may be ahead of the pixel stage (24) */
	int timeout_ms;                 /* an SPE that does not answer this long stops the decoder (5000) */
	uint32_t records_bytes;         /* the CABAC stage's output ring (32 MB) */
	int max_pictures;               /* decode at most this many */
	const uint32_t *check_ref;      /* hash every picture and compare it with check_ref[decode index] */
	int check_n;
	int check;                      /* hash every picture (statistics), with or without a reference */
	int stats_pictures;             /* per-picture statistics kept, decode order (256) */
} tdec_config;

typedef struct {
	const uint8_t *y, *u, *v;       /* the visible picture's top left sample (the crop window applied) */
	int stride_y, stride_c;         /* bytes from one row to the next */
	int width, height;              /* the visible size, e.g. 1920 x 1080; chroma planes: half of each */
	int64_t pts;                    /* the time stamp fed with its access unit */
	int index;                      /* display order: 0, 1, 2 ... since tdec_open / tdec_flush */
	int decode_index;               /* decode order (the order fed, pictures dropped at the start not counted) */
	uint32_t offset_y, offset_u, offset_v;   /* y, u, v as byte offsets into the picture memory block */
	int out_, gen_;                 /* internal */
} tdec_picture;

void   tdec_default(tdec_config *c);
size_t tdec_picture_memory_size(const tdec_config *c);   /* whole megabytes */

tdec  *tdec_open(const tdec_config *c, char *err, int errlen);
void   tdec_close(tdec *d);

/* 1 taken (copied: the caller's buffer is free again at once), 0 the input is full - try again a little
   later, -1 never (an unknown format, or larger than 2 MB) */
int    tdec_feed(tdec *d, const void *au, size_t size, int format, int64_t pts);
/* the avcC box's contents (an MP4's or MKV's codec private data): its SPS and PPS go to the decoder.
   Returns the NAL length size of the samples (the format for tdec_feed: 1, 2 or 4), 0 the input is full,
   -1 not an avcC */
int    tdec_feed_avcc_config(tdec *d, const void *avcc, size_t size);
void   tdec_end_of_stream(tdec *d);

/* 1 the next picture in display order, 0 none decoded yet, -1 the end (after tdec_end_of_stream, every
   picture handed out) or an error (tdec_error) */
int    tdec_get_picture(tdec *d, tdec_picture *pic);
void   tdec_release_picture(tdec *d, const tdec_picture *pic);
/* the picture packed into one buffer: Y (width x height), then U, then V (half size each) - what a player
   that uploads I420 wants; dst holds width * height * 3 / 2 bytes */
void   tdec_copy_i420(const tdec_picture *pic, uint8_t *dst);

/* seek: everything fed and not shown is dropped, the SPEs restart (a few ms). Release every picture
   first; feed the new position from a key frame on. 0 ok, -1 failed (tdec_error) */
int    tdec_flush(tdec *d);

const char *tdec_error(const tdec *d);         /* NULL while all is well */
void   tdec_picture_memory(const tdec *d, void **base, size_t *size);
int    tdec_size(const tdec *d, int *width, int *height);   /* the visible size; 0 before the first picture */
int    tdec_pictures_decoded(const tdec *d);   /* since tdec_open / tdec_flush */

/* Annex B (.264, MPEG-TS payloads): the next access unit - one picture's NAL units, start codes included - at
   or after *pos in data. 1 with [*au, *au + *len), *pos moved past it; 0 at the end. A new access unit starts at
   an access unit delimiter, SEI, SPS or PPS after a slice, or at a slice with first_mb_in_slice 0. The last
   one of a buffer is only complete once the buffer holds the stream's end or the next picture's start. */
int    tdec_next_au(const uint8_t *data, size_t size, size_t *pos, const uint8_t **au, size_t *len);

/* tests: the decoder core underneath (a decoder * of decoder.h: per-picture and per-SPE statistics) */
void  *tdec_core(tdec *d);

#ifdef __cplusplus
}
#endif

#endif

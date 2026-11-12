/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/tee_vp9.h; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9 - a VP9 decoder of its own on the PS3's SPEs, as a building block for apps. The interface follows the TEE
   Decoder's (tee_decoder.h, H.264) function by function, so a player that drives one can drive the other.

   What it decodes: VP9 profile 0 (8 bit, 4:2:0), up to 2560 x 1440, as YouTube serves it in WebM (itag 308, 271,
   303, 248 ...). Not: profiles 1 - 3 (10 bit, 4:4:4), a reference frame of another size (scaled prediction). Such
   a stream stops with an error (tvp9_error says which), it never shows garbage.

   How fast (measured on the console with the test application, whole clips, every picture equal to ffmpeg's): real
   YouTube 1440p60 at 62 - 66 pictures a second on 2 entropy + 4 pixel SPEs, all 6; 74 - 77 with the RSX prediction
   (tvp9_config.rsx - an option that asks the app's display code to share the RSX: tv9_rsx_host.h).

   Use - one decoder at a time:

     tvp9_config c;
     tvp9_default(&c);                                   2 + 4 SPEs, up to 2560 x 1440
     tvp9 *d = tvp9_open(&c, err, sizeof(err));          the SPEs start (NULL: err says why)

     feeding thread:
       for every block of the video track (a WebM SimpleBlock's or Block's payload, in the file's order):
         while ((r = tvp9_feed(d, data, size, pts)) == 0)
           sleep a millisecond;                          the input is full: the decoder is ahead
       tvp9_end_of_stream(d);                            the rest comes out, then tvp9_get_picture says -1

     display thread (60 times a second or whenever it wants one):
       if (tvp9_get_picture(d, &pic) == 1) {             the next picture
         draw pic.y / pic.u / pic.v ...                  its planes stay valid until it is released
         tvp9_release_picture(d, &pic);                  when the screen (the RSX) is done with it
       }

     seek:  stop feeding, release every picture, tvp9_flush(d), then feed from a key frame on
     end:   tvp9_close(d)                                the SPEs stop

   Threads: tvp9_feed and tvp9_end_of_stream from one thread at a time; tvp9_get_picture and tvp9_release_picture
   from one thread at a time (the same or another). tvp9_flush and tvp9_close only while nothing is being fed. The
   decoder runs its own PPU thread (and the SPEs) in between.

   Blocks and pictures: VP9 has no reordering - the pictures come out in the order their blocks were fed, each with
   its block's time stamp. A block is one frame or a superframe (a hidden frame packed in front of a shown one: the
   decoder splits it). A block gives one picture or none (a frame that is only a reference). A stream may start
   anywhere: blocks before the first key frame are dropped. tvp9_probe tells a key frame and its size without a
   decoder.

   Pictures: the decoder writes them straight into its picture buffers (one memory block, see tvp9_picture_memory);
   an app can draw them from there without a copy - map the block for the RSX once and use pic.offset_y / _u / _v -
   or copy them out (tvp9_copy_i420). A 1440p picture is 5.5 MB: at 60 a second a copy is 330 MB/s, so draw from the
   block. Hold at most 3 at a time: a held picture keeps its buffer, and with too few free buffers the decoder
   waits. A buffer's rows are stride bytes apart and the picture has a border around it (80 samples, chroma 48):
   the planes are not packed.

   SPEs: the decoder takes entropy_spes + pixel_spes SPU threads, 6 by default - all there are (a 7th froze the
   console). Sound that runs on an SPE (cellAdec) has no room beside the default; it has to run on the PPU, or the
   decoder gets fewer pixel SPEs and is slower.

   Memory: tvp9_picture_memory_size + tvp9_memory_size bytes for a configuration, before the SPEs start. */
#ifndef TEE_VP9_H
#define TEE_VP9_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEE_VP9_VERSION "0.2.2"

typedef struct tvp9 tvp9;

typedef struct {
	int entropy_spes;               /* 2 (default) or 1: the entropy stage's SPEs */
	int pixel_spes;                 /* 4 (default), 2 or 3: the pixel stage's SPEs */
	int max_width, max_height;      /* the largest stream it is set up for (default 2560 x 1440) */
	int pictures;                   /* picture buffers (default 10 = 65 MB at 1440p): the stream's reference frames (3 - 4
	                                   with YouTube, 8 at most), the frames being decoded and the pictures the app holds */
	uint32_t input_bytes;           /* blocks fed and not parsed yet (default 6 MB) */
	void *picture_memory;           /* optional: the picture buffers' block, 1 MB aligned and at least
	                                   tvp9_picture_memory_size(&config) bytes (e.g. one the app has mapped for the
	                                   RSX); NULL: the decoder allocates it (tvp9_picture_memory says where) */
	size_t picture_memory_bytes;
	void (*log)(const char *line);  /* optional: the decoder's log lines (any thread) */

	/* for tests; 0 = the default */
	int lead;                       /* frames the entropy stage may be ahead of the pixel stage (3) */
	int tasks;                      /* frames with their records ready or in the pixel stage (5) */
	int timeout_ms;                 /* an SPE that does not answer this long stops the decoder (10000) */
	int max_pictures;               /* decode at most this many */
	int no_store;                   /* the pixel SPEs write reconstructed superblocks into the frame, not a store */

	/* 0.2: 1 = the RSX predicts the inter blocks that have one reference, and the pixel SPEs fetch its predictions
	   (real YouTube 1440p60: 74 - 77 pictures a second instead of 62 - 66). Needs a build with the block's RSX part
	   (-DTV9_RSX: rsx_mc.c, vp9_rsxmc.c, vp9_tables.c, tv9_rsx_programs.c, and the app's display code as
	   tv9_rsx_host.h asks) - without it tvp9_open fails.
	   If the RSX stops taking the block's commands while a stream plays (2 s without a row), the block gives the RSX
	   up until tvp9_close and goes on on the SPEs alone. The frames that were under way have no proper predictions
	   then, and the frames after them build on those: their pictures are not handed out, up to the next key frame
	   (tvp9_picture.index skips their numbers; YouTube has a key frame about every 5 s).
	   The picture block is mapped for the RSX by the decoder, and 17 MB of prediction surfaces come on top */
	int rsx;
} tvp9_config;

typedef struct {
	const uint8_t *y, *u, *v;       /* the picture's top left sample */
	int stride_y, stride_c;         /* bytes from one row to the next */
	int width, height;              /* the picture's size, e.g. 2560 x 1440; chroma planes: half of each, rounded up */
	int64_t pts;                    /* the time stamp fed with its block */
	int index;                      /* 0, 1, 2 ... since tvp9_open / tvp9_flush (numbers are skipped where pictures
	                                   were not handed out after the RSX part failed: see tvp9_config.rsx) */
	int key_frame;                  /* 1: a key frame */
	uint32_t offset_y, offset_u, offset_v;   /* y, u, v as byte offsets into the picture memory block */
	int out_, gen_;                 /* internal */
} tvp9_picture;

void   tvp9_default(tvp9_config *c);
size_t tvp9_picture_memory_size(const tvp9_config *c);   /* whole megabytes */
size_t tvp9_memory_size(const tvp9_config *c);           /* what the decoder allocates besides the picture block */

tvp9  *tvp9_open(const tvp9_config *c, char *err, int errlen);
void   tvp9_close(tvp9 *d);

/* a block of the video track: 1 taken (copied: the caller's buffer is free again at once), 0 the input is full - try
   again a little later, -1 never (empty, or larger than the input) */
int    tvp9_feed(tvp9 *d, const void *block, size_t size, int64_t pts);
void   tvp9_end_of_stream(tvp9 *d);

/* 1 the next picture, 0 none decoded yet, -1 the end (after tvp9_end_of_stream, every picture handed out) or an
   error (tvp9_error) */
int    tvp9_get_picture(tvp9 *d, tvp9_picture *pic);
void   tvp9_release_picture(tvp9 *d, const tvp9_picture *pic);
/* the picture packed into one buffer: Y (width x height), then U, then V (half size each, rounded up) - dst holds
   width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2) bytes */
void   tvp9_copy_i420(const tvp9_picture *pic, uint8_t *dst);

/* seek: everything fed and not shown is dropped. Release every picture first; feed the new position from a key
   frame on (earlier blocks are dropped). A stream of another size may follow a flush. 0 ok, -1 failed (tvp9_error)

   (0.2.2) A change of quality needs no flush: feed the other stream from one of its key frames right after the last
   block of the first one (a key frame of another size, within max_width x max_height). The decoder lets the frames
   before it finish, then goes on at the new size; no picture is lost, those of the old size keep their planes and
   size until released. The change costs the pipeline's depth once - about 5 frames of decoding time. A frame of
   another size that is not a key frame is still refused. */
int    tvp9_flush(tvp9 *d);

const char *tvp9_error(const tvp9 *d);         /* NULL while all is well */
void   tvp9_picture_memory(const tvp9 *d, void **base, size_t *size);
int    tvp9_size(const tvp9 *d, int *width, int *height);   /* the picture size; 0 before the first key frame */
int    tvp9_pictures_decoded(const tvp9 *d);   /* since tvp9_open / tvp9_flush */

/* a block's first frame, without a decoder: 1 a key frame (*width, *height and *profile set), 0 another frame
   (*profile set), -1 not a VP9 frame. For a demuxer that looks for a place to start and for a player that asks
   whether the decoder takes the stream (profile 0, the size within the configuration) */
int    tvp9_probe(const void *block, size_t size, int *width, int *height, int *profile);

/* what the decoder did, for an overlay or a log: all since tvp9_open / tvp9_flush */
typedef struct {
	int frames, pictures;           /* frames decoded (hidden ones too); pictures handed out */
	int dropped;                    /* blocks dropped before the first key frame */
	int rsx_frames;                 /* frames the RSX predicted blocks of (tvp9_config.rsx) */
	double entropy_ms, pixel_ms;    /* the mean time a frame took on its entropy SPEs (the slower one) and on its
	                                   pixel SPEs (their sum) */
	double fps;                     /* frames decoded a second of wall time while the decoder was not waiting for
	                                   input or for the app to release pictures */
} tvp9_stats;
void   tvp9_get_stats(const tvp9 *d, tvp9_stats *s);

#ifdef __cplusplus
}
#endif

#endif

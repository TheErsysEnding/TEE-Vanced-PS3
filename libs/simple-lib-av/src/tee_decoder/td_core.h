/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/decoder.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder: the decoder core on the PPU - from H.264 access units, fed one by one (dec_feed), to decoded
   pictures in output order, with the work on SPEs:
     input      (any thread) access units with their time stamps into the input ring (Annex B or MP4's
                length-prefixed NAL units)
     parse      (decoder thread) NAL units, SPS / PPS / slice header, the slice data unescaped into the
                RBSP ring; the picture buffers are laid out at the first slice (or the first after a restart)
     CABAC      (CABAC SPEs) one picture per job, its records into the records ring; up to `lead` pictures
                ahead of the pixel stage (an IDR picture's CABAC takes several picture times)
     pixel      (pixel SPEs) one picture per job, several pictures at once; a job waits for the rows of its
                references that it needs (frame threading, see pixel_job.h)
     output     the pictures in output order (POC, max_num_reorder_frames) to the display thread
     check      (hash thread) every picture hashed and compared with the Mac's reference
   Portable: the same code runs in the Mac simulation (plat.h). */
#ifndef DECODER_H
#define DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "td_dpb.h"
#include "td_pixel_job.h"
#include "td_recon.h"

#define DEC_OUTQ      64
#define DEC_MAX_CABAC 2

typedef struct {
	int ncabac;                     /* CABAC SPEs: 1 or 2 */
	int npixel;                     /* pixel SPEs: 1..5, ncabac + npixel <= 6 */
	int lead;                       /* pictures the CABAC stage may be ahead of the pixel stage */
	int check;                      /* hash every picture and compare it with the reference */
	int max_pictures;               /* decode at most this many (0: all) */
	int timeout_ms;                 /* an SPE that does not answer for this long: the run is stopped */
} dec_cfg;

/* per picture in decode order. t_*: PPU timebase (plat_ticks); SPU times: decrementer ticks (same rate) */
typedef struct {
	int8_t type;                    /* 0 P, 1 B, 2 I */
	int8_t cabac_spe, pixel_spe;    /* which SPE of its kind */
	int8_t hash_ok;                 /* -1 not checked, 0 differs, 1 equal to the reference */
	int32_t poc, out_idx;
	uint32_t slice_bytes, rec_bytes;
	uint32_t cabac_ticks, cabac_wait, cabac_tries;
	uint32_t pix_ticks, pix_rows, pix_polls, fetch_bytes;
	uint32_t parts[PIX_PARTS];
	uint32_t aprof[PIX_APROF];      /* stage A's motion by macroblock kind (pixel_job.h) */
	uint32_t hash;
	uint64_t t_parse, t_cabac_in, t_cabac_out, t_pix_in, t_pix_out, t_hash;
	int32_t vb_due, vb_shown;       /* display (vblanks since the run's first picture), vb_shown -1: dropped */
} dec_picstat;

/* the output queue the display thread reads, output index k at [k % DEC_OUTQ] */
typedef struct {
	volatile int32_t state;         /* 0 free, 1 decoded (the display may use it), 2 the display is done with it */
	int32_t pic, slot, pad;
	int64_t pts;                    /* as fed with its access unit */
	int64_t pad2;
} dec_out;

/* access unit formats for dec_feed */
#define DEC_AU_ANNEXB 0             /* start codes (00 00 01) */
/* 1, 2 or 4: NAL units with a big-endian length of that many bytes in front (MP4: avcC lengthSizeMinusOne + 1) */

/* per SPE and run */
typedef struct {
	int kind;                       /* 0 CABAC, 1 pixel */
	uint32_t jobs;
	uint64_t busy;                  /* decrementer ticks inside jobs */
	uint64_t waited;                /* of those: waiting for other SPEs (pixel: references) or DMA (CABAC) */
} dec_spestat;

typedef struct decoder decoder;

typedef struct {
	int slots;                      /* picture buffers (raised to the stream's references + 4, at most DPB_SLOTS) */
	uint32_t rec_ring;              /* bytes of the records ring, a power of two */
	uint32_t in_bytes;              /* bytes of the input ring (access units not parsed yet) */
	int max_width, max_height;      /* the largest picture the buffers are made for (at most 1920 x 1088) */
	void *dpb_block;                /* the picture buffers' memory, 1 MB aligned (e.g. mapped for the RSX once), */
	size_t dpb_bytes;               /* or NULL: the decoder allocates it */
	const uint32_t *ref;            /* check hashes per picture (decode order), or NULL */
	int nref;
	int stats_pictures;             /* per-picture statistics kept (decode order); a longer stream keeps the last ones */
} dec_setup;

decoder *dec_create(const dec_setup *su, char *err, int errlen);
void dec_destroy(decoder *d);
size_t dec_dpb_bytes_for(int max_width, int max_height, int slots);    /* what a dpb_block needs (whole MB) */

/* The input, from one thread at a time: one access unit (a picture's NAL units) and its time stamp, copied
   into the input ring. 1 taken, 0 the ring is full (try again later), -1 it can never fit or the format is
   unknown. dec_feed_end: no more access units - the decoder finishes what it has. */
int  dec_feed(decoder *d, const uint8_t *au, size_t size, int format, int64_t pts);
void dec_feed_end(decoder *d);

/* the picture buffers: one block (the RSX maps it), a slot's planes, the crop window (known once the first
   slice was parsed; dec_geometry 0 before) */
void dec_dpb_block(const decoder *d, void **base, size_t *size);
const recon_pic *dec_slot_pic(const decoder *d, int slot);
void dec_crop(const decoder *d, int *x, int *y, int *w, int *h);
int  dec_geometry(const decoder *d, int *width, int *height);
int  dec_slots(const decoder *d);

/* a run: SPEs started, the decoder thread (and the hash thread) running. The display thread reads
   dec_outq() in output order and sets every entry it is done with to state 2. */
int  dec_start(decoder *d, const dec_cfg *cfg);
dec_out *dec_outq(decoder *d);
int  dec_out_total(const decoder *d);           /* output pictures of this run, -1 until known */
int  dec_done(const decoder *d);                /* the decoder thread has finished (or failed) */
const char *dec_error(const decoder *d);        /* NULL if all went well */
void dec_wake(decoder *d);                      /* the display thread gave something back */
void dec_stop(decoder *d);                      /* threads joined, SPEs stopped */

/* per-picture statistics, picture k (decode order) at [k % stats capacity]; *n: the pictures done */
const dec_picstat *dec_stats(const decoder *d, int *n);
dec_picstat *dec_stats_rw(decoder *d);          /* the display thread writes vb_due / vb_shown */
int  dec_stats_cap(const decoder *d);
const dec_spestat *dec_spe_stats(const decoder *d, int *n);
uint64_t dec_run_ticks(const decoder *d);       /* from dec_start to the last picture done */
int  dec_pictures_done(const decoder *d);

#endif

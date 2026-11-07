/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/pixel_job.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder: one picture's reconstruction for a pixel SPE and its answer. Shared by the PPU
   (source/decoder.c), the SPU program (spu_pixel/source/main.c) and the Mac simulation; all big-endian
   with the same alignment on the console, plain C structs on the Mac.

   Several pixel SPEs decode different pictures at the same time (frame threading). Every picture has a
   progress line (128 bytes, the first word counts): luma lines [0, P) are final - deblocked, and for a
   reference picture with their left / right borders; P >= PIX_PROG_ROW0 also means the top border is
   there; PIX_PROG_DONE: the whole picture, bottom border included, and its motion field. A job waits for
   its references' progress before it fetches a reference block (and, in B pictures, before it reads the
   colocated motion field of RefPicList1[0]). */
#ifndef PIXEL_JOB_H
#define PIXEL_JOB_H

#include <stdint.h>

#define PIX_EVENT_PORT   13
#define PIX_MAX_REFS     16

/* job flags */
#define PIX_DEBLOCK      1           /* the deblocking filter (disable_deblocking_filter_idc != 1) */
#define PIX_BORDERS      2           /* a reference picture: extend its edges into the border */
#define PIX_PROGRESS     4           /* publish progress (pictures others refer to) */
#define PIX_WAITREFS     8           /* wait for the references' progress (else: they are complete) */

#define PIX_PROG_ROW0    13          /* after macroblock row r < last: 16 r + 13 */
#define PIX_PROG_DONE    0x7FFFFFFFu

typedef struct {
	uint32_t y, u, v;                /* EA of sample (0, 0) */
	int32_t poc, long_term, id;
	uint32_t prog;                   /* EA of the picture's progress line; 0: complete */
	uint32_t pad;
} pix_ref;                           /* 32 bytes */

typedef struct {
	uint32_t seq, flags;
	uint32_t rec_ea, rec_ring;       /* the CABAC records ring: base (128-aligned), size (a power of two) */
	uint32_t rec_off, rec_bytes;     /* this picture's records: offset in the ring (128-aligned), bytes (multiple of 16) */
	uint32_t out_y, out_u, out_v;    /* the picture being made, sample (0, 0) */
	uint32_t stride_y, stride_c;
	uint32_t col_out, col_in;        /* recon_col arrays: this picture's, RefPicList1[0]'s (B) */
	uint32_t prog_ea;                /* this picture's progress line (PIX_PROGRESS) */
	uint32_t result_ea;
	int32_t slice_type, mb_w, mb_h, cur_poc, out_id;
	int32_t num_ref[2];
	int32_t direct_spatial, wp_mode, luma_log2, chroma_log2;
	int32_t cqp_offset[2], disable_deblock, alpha_offset, beta_offset;
	int32_t pad[3];
	pix_ref ref[2][PIX_MAX_REFS];    /* 1 KB */
	int16_t lw[2][PIX_MAX_REFS], lo[2][PIX_MAX_REFS];
	int16_t cw[2][PIX_MAX_REFS][2], co[2][PIX_MAX_REFS][2];
} __attribute__((aligned(128))) pix_job;

enum { PIX_OK = 0, PIX_BAD_JOB = 1, PIX_RECON = 2, PIX_FETCH_FULL = 3 };

/* parts: the order of recon.h's RT_* */
#define PIX_PARTS 16
#define PIX_APROF 10

typedef struct {
	uint32_t seq, status, error, mbs;
	uint32_t ticks_total;            /* decrementer ticks from taking the job to the answer */
	uint32_t ticks_rows;             /* in row_end: deblocking + borders + progress, waits included */
	uint32_t ref_polls;              /* progress reads that found too little */
	uint32_t pad0;
	uint32_t parts[PIX_PARTS];       /* decrementer ticks per RT_* part */
	uint32_t fetch_bytes, fetch_rows;
	uint32_t aprof[PIX_APROF];       /* RT_M_PSKIP .. RT_N_COLWAIT: stage A's motion by macroblock kind, colocated waits */
} __attribute__((aligned(128))) pix_result;

#define PIX_ANSWER(seq, status)  ((((seq) & 0xFFFF) << 8) | ((status) & 0xFF))

#endif

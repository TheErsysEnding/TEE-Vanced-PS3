/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/vp9_job.h; do not edit here: change the
   project's file and run the script again. */
/* One VP9 frame for an SPU and its answer. Shared by the PPU (source/test.c) and the SPU program (spu/source/main.c);
   both are big-endian with the same alignment, so the layout matches. */
#ifndef VP9_JOB_H
#define VP9_JOB_H

#include <stdint.h>

#define VP9_EVENT_PORT    12
#define VP9_MV_SB_BYTES   1024          /* a superblock's 64 vectors (vp9_mvref, 16 bytes) in the vector buffers */
#define VP9_XCHG_STRIDE   16384         /* the count exchange: per frame parity and SPU 16 KB of counts, then the flags */
#define VP9_XCHG_BYTES    (4 * VP9_XCHG_STRIDE + 384)   /* + the two SPUs' flags, the tile column counter */
#define VP9_RES_TILES     16

/* job flags */
#define JOB_NO_OUTPUT     1             /* the records are written in the local store but not sent */
#define JOB_RESET         2             /* a stream's first frame: a fresh parser */
#define JOB_MICRO         8             /* isolated local-store diagnostic, output is vp9_bench_result */
#define JOB_STRESS        4             /* no frame: stream data_size bytes at data_ea in and out until the word at xchg_ea is not 0 */

typedef struct {
	uint32_t seq, flags;
	uint32_t frame_size;                /* the frame's bytes */
	uint32_t data_size;                 /* bytes at data_ea: at least frame_size, a multiple of 16 */
	uint64_t data_ea;                   /* the frame, 128-byte aligned */
	uint64_t out_ea;                    /* the records, 128-byte aligned */
	uint32_t out_cap;
	uint32_t part, parts;               /* this SPU's share of the tile columns: part of parts (1 or 2) */
	uint32_t fseq;                      /* the frame's number, the same for both SPUs of a frame, rising over the whole test */
	uint64_t mv_ea[2];                  /* two vector buffers (this frame's, the previous frame's), 128-byte aligned */
	uint64_t xchg_ea;                   /* parts 2: VP9_XCHG_BYTES where the two SPUs swap their counts */
	uint64_t result_ea;                 /* vp9_result */
	uint32_t mv_bytes;                  /* each vector buffer's size */
	uint32_t pad1;
	uint64_t rowoff_ea;                 /* 0, or VP9_ROWOFF_BYTES: per tile (raster index) VP9_ROWOFF_TILE bytes of uint32,
	                                       one per superblock row (frame row index): where the row's records start in this
	                                       SPU's output - the pixel stage gets the records superblock row by row */
	uint32_t pad[2];
} __attribute__((aligned(128))) vp9_job;

#define VP9_ROWOFF_TILE   256           /* 64 superblock rows: frames up to 4096 lines */
#define VP9_ROWOFF_BYTES  (VP9_RES_TILES * VP9_ROWOFF_TILE)

enum { RES_OK = 0, RES_BAD_JOB = 1, RES_OUT_FULL = 2, RES_PARSE_ERROR = 3, RES_XCHG_TIMEOUT = 4 };

typedef struct {
	uint32_t seq, status, error, error_a, error_b;
	/* SPU decrementer ticks (the timebase, 79.8 MHz on the PS3) */
	uint32_t ticks_total;               /* from taking the job to the last record sent */
	uint32_t ticks_headers;             /* the uncompressed and the compressed header */
	uint32_t ticks_tiles;               /* this SPU's tiles (DMA waits included) */
	uint32_t ticks_xchg;                /* parts 2: sending the counts, waiting for the other SPU's, adding them */
	uint32_t ticks_finish;              /* adaptation, references */
	uint32_t ticks_wait_in;             /* waiting for bitstream DMA */
	uint32_t ticks_wait_out;            /* waiting for record DMA */
	uint32_t ticks_wait_mv;             /* waiting for vector DMA */
	uint32_t ticks_idle;                /* waiting for this job after answering the one before */
	uint32_t ticks_answer;              /* answering the job before */
	uint32_t out_bytes, blocks, tx_blocks, coefs;
	uint32_t ticks_modes, ticks_tokens, ticks_rest;   /* in the blocks: mode info, transform data, the rest */
	uint32_t frame_type, show_frame, show_existing, intra_only, width, height;
	uint32_t ntiles;                    /* this part's tiles: their raster index, where their records start */
	uint16_t tile_idx[VP9_RES_TILES];
	uint32_t tile_off[VP9_RES_TILES];
	uint32_t slack_max;
	uint32_t stress_mb;                 /* JOB_STRESS: megabytes moved (in and out) */
	/* the headers, for the pixel stage */
	uint8_t lf_level, lf_sharpness, lf_delta_enabled, lossless;
	int8_t lf_ref_deltas[4], lf_mode_deltas[2];
	uint8_t ref_frame_idx[3], refresh_frame_flags, existing_idx, pad8;
} __attribute__((aligned(128))) vp9_result;

/* the answer event's data0 (24 bits): (seq & 0xFFFF) << 8 | status */
#define VP9_ANSWER(seq, status)  ((((seq) & 0xFFFF) << 8) | ((status) & 0xFF))

#endif

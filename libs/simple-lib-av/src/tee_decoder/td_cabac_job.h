/* TEE Decoder building block - made by tools/make_baustein.sh from ps3/source/cabac_job.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder: one picture's slice data for a CABAC SPE and its answer. Shared by the PPU
   (source/decoder.c), the SPU program (spu_cabac/source/main.c) and the Mac simulation; on the console both
   sides are big-endian with the same alignment. From TEE CABAC SPU Test, with the output going into the
   records ring the pixel SPEs read from. */
#ifndef CABAC_JOB_H
#define CABAC_JOB_H

#include <stdint.h>

#include "td_cabac_slice.h"

#define CABAC_EVENT_PORT  12

/* job flags */
#define JOB_NO_OUTPUT     1      /* parse only: the output stream is dropped instead of sent */

typedef struct {
	uint32_t seq, flags;
	uint32_t data_size;          /* bytes at data_ea, a multiple of 16, zero padding included */
	uint32_t out_cap;            /* bytes this picture may use in the ring */
	uint64_t data_ea;            /* slice data from its first CABAC byte, 128-byte aligned */
	uint64_t out_ea;             /* the records ring, 128-byte aligned */
	uint64_t result_ea;          /* cabac_result */
	uint32_t out_off;            /* where this picture's records start in the ring (128-aligned) */
	uint32_t out_ring;           /* the ring's size, a power of two */
	cabac_slice_params prm;      /* 64 bytes */
} __attribute__((aligned(128))) cabac_job;

enum { RES_OK = 0, RES_BAD_JOB = 1, RES_OUT_FULL = 2 };

typedef struct {
	cabac_stats st;              /* 128 bytes; the bin counters stay 0 in the timing build */
	uint32_t seq, status;
	/* SPU decrementer ticks (the timebase, 79.8 MHz on the PS3) */
	uint32_t ticks_total;        /* from taking the job to the last output byte sent */
	uint32_t ticks_wait_in;      /* waiting for bitstream DMA */
	uint32_t ticks_wait_out;     /* waiting for output DMA */
	uint32_t ticks_idle;         /* waiting for this job after answering the one before */
	uint32_t ticks_answer;       /* answering the job before (result DMA and event) */
	uint32_t out_bytes;          /* the records' size (also when they did not fit: out_cap was too small) */
	uint32_t pad[20];
} __attribute__((aligned(128))) cabac_result;

/* the answer event's data0 (24 bits): (seq & 0xFFFF) << 8 | status */
#define CABAC_ANSWER(seq, status)  ((((seq) & 0xFFFF) << 8) | ((status) & 0xFF))

#endif

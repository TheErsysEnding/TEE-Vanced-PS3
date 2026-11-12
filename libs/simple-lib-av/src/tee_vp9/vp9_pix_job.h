/* TEE VP9 building block - made by tools/make_baustein.sh from ps3/source/vp9_pix_job.h; do not edit here: change the
   project's file and run the script again. */
/* One VP9 frame for the pixel SPU (spu_pixel/source/main.c) and its answer. Shared by the PPU (source/test.c) and the
   SPU program; both are big-endian with the same alignment, so the layout matches.

   Frame buffers (the PPU allocates them): every plane is its frame size aligned to 64 (luma) / 32 (chroma) with a border
   of PIX_BORDER_Y / PIX_BORDER_C samples on each side; rows are stride bytes apart, the top-left visible sample is 16-byte
   aligned. Reference frames get their borders filled (pass 3) with the edge samples, so reading anywhere in the border
   is the same as clamping the coordinates, as VP9's prediction requires. */
#ifndef VP9_PIX_JOB_H
#define VP9_PIX_JOB_H

#include <stdint.h>

#define PIX_EVENT_PORT  13
#define PIX_BORDER_Y    80
#define PIX_BORDER_C    48
#define PIX_LFM_BYTES   176             /* one superblock's loop filter mask (vp9_lf_mask, padded to 16) */

typedef struct {
	uint64_t y, u, v;                   /* the frame's top-left visible sample */
	uint64_t pad;
} pix_frame;

/* job flags */
#define PIXJ_BORDERS    1               /* a reference frame: fill its borders (pass 3) */
#define PIXJ_NO_LF      2               /* skip the loop filter (a test of the other passes) */
#define PIXJ_SELFTEST   4               /* no frame: compare the SIMD kernels with libvpx's C ones on generated input */
#define PIXJ_C_MC       8               /* inter prediction with libvpx's C convolution instead of the SIMD one */
#define PIXJ_C_LPF      16              /* the loop filter with libvpx's C instead of the SIMD one */
#define PIXJ_C_TX       32              /* the inverse transforms with libvpx's C instead of the SIMD ones */
#define PIXJ_C_IP       64              /* intra prediction with libvpx's C predictors instead of the SIMD ones */
#define PIXJ_LF_NEW     128             /* the loop filter's version 2 (simd_lpf.h: the runs listed, then filtered inline) */
#define PIXJ_PART_REC   256             /* a big frame on two SPUs: this job reconstructs it (pass 1), its progress at rec_ea */
#define PIXJ_PART_LF    512             /* ... this one loop filters it and fills its borders, a superblock row behind */
#define PIXJ_HALF_A     2048            /* a frame split by tile columns: this job decodes the columns left of job.seam */
#define PIXJ_HALF_B     4096            /* ... this one those from job.seam on (and the borders and the frame's progress) */
#define PIXJ_LF_LAG2    32768           /* a split frame's halves: a row's loop filter two superblock rows behind the
                                           reconstruction (not one), so the halves have a row of room against each other */
#define PIXJ_C_LFM      16384           /* the loop filter masks by libvpx's vp9_setup_mask at a superblock's end (not
                                           block by block) */
#define PIXJ_RSX_PRED   65536           /* the RSX predicts the inter blocks with one reference (common/vp9_rsxmc.h): their
                                           predictions come per superblock from job.pred, once job.pred_lab_ea's label
                                           has counted the superblock row; compound blocks are predicted here as before */
#define PIXJ_RSX_STRIPS 2097152         /* with PIXJ_RSX_PRED: job.pred.y is a surface of cells, one of PIX_STRIP_W x
                                           PIX_STRIP_H bytes a superblock, PIX_STRIP_PER side by side (pitch
                                           pred_pitch_y): superblock n = row * superblock columns + column at
                                           ((n % PER) * W, (n / PER) * H). A cell's rows: luma rows 0..31 in bytes 0..63,
                                           luma rows 32..63 in bytes 64..127, U in 128..159, V in 160..191 (rsxmc_geom.strips):
                                           fetched with a list of 32 elements instead of 128 */
#define PIX_STRIP_W     192
#define PIX_STRIP_H     32
#define PIX_STRIP_PER   12
#define PIXJ_RSX_COMPOUND 1048576       /* with PIXJ_RSX_PRED: the RSX predicts the blocks with two references too (those of
                                           the sizes in rsx_sizes; rsxmc_geom.compound) */
#define PIXJ_RSX_MARKER 524288          /* with PIXJ_RSX_PRED: pred_lab_ea is not the RSX's label but a marker in main memory,
                                           16 bytes the RSX draws after every row (through the path the predictions take):
                                           byte 0 = (pred_lab_base & 7) << 5 | (the row + 1), byte j = byte 0 + j */
#define PIXJ_RSX_REREAD 262144          /* with PIXJ_RSX_PRED, a debug check: the first two superblocks of every row (those read
                                           right after the row's label) are read again when the row is done; rsx_chk_* count
                                           those that changed in between */
#define PIXJ_RSX_CHECK  131072          /* with PIXJ_RSX_PRED, a debug check: no predictions asked for ahead; the SPU predicts
                                           every inter block itself as well and compares the RSX's bytes with its own (the
                                           frame is decoded from its own); every superblock's predictions are read again at
                                           its end and compared with what was read at its start */
#define PIXJ_HALF_LAZY  8192            /* ... a row's loop filter waits for the other half only when nothing else is left:
                                           the reconstruction goes on and the filter catches up (it may lag any number of
                                           rows: row k's filter touches rows k - 1 and k only, row k + 1's prediction
                                           reads row k unfiltered) */

#define PIXJ_SBSTORE    16777216        /* (3.57) the reconstructed superblocks go to a store of their own before the loop
                                           filter, not into the frame: superblock (r, c) as one piece of PIX_STORE_SB bytes
                                           (the SPU's superblock buffer as it is) at store_ea + ((r mod store_rows) *
                                           columns + c) * PIX_STORE_SB - one transfer where the frame takes a list of 128
                                           rows, and the loop filter fetches it back with three instead of 152. The frame
                                           gets the filtered pixels only. A ring of store_rows superblock rows (a power of
                                           two, 4 at least): the job's loop filter is never more than store_rows - 2 rows
                                           behind its reconstruction. Not with PIXJ_PART_*, ignored without loop filter */
#define PIX_STORE_SB    8448            /* sizeof the pixel SPU's superblock buffer: luma 65 x 80, chroma 2 x 33 x 48 */
#ifndef PIX_DIAG
#define PIX_DIAG        0               /* 1: the two measurement switches below are built in (the pixel SPU: 700 bytes;
                                           3.56 was built so) */
#endif
#define PIXJ_DIAG_OUT2  4194304         /* a measurement, same result: every superblock's reconstruction is written out
                                           twice (128 more list elements and 6 KB a superblock for the SPU's MFC) */
#define PIXJ_DIAG_LF2   8388608         /* ... every superblock's loop filter window is fetched twice (152 more elements,
                                           7.7 KB) */

typedef struct {
	uint32_t seq, flags;
	uint32_t width, height, aw, ah;     /* the frame size and the luma size aligned to 64 */
	uint32_t stride_y, stride_c;
	uint32_t intra_frame, lossless;
	uint32_t lf_level, lf_sharpness, lf_delta_enabled;
	int32_t lf_ref_deltas[4], lf_mode_deltas[2];
	uint32_t records_bytes;
	uint64_t records_ea;                /* the entropy stage's records of this frame, 128-byte aligned */
	uint64_t lfm_ea;                    /* scratch: sb64_rows * sb64_cols * PIX_LFM_BYTES */
	uint64_t result_ea;
	pix_frame cur, ref[3];              /* this frame; LAST, GOLDEN, ALTREF */
	uint64_t prog_ea;                   /* this frame's progress (pix_prog, 128-byte aligned), which other jobs read */
	uint64_t ref_prog_ea[3];            /* the references' progress: their windows wait for the rows they need */
	uint64_t rec_ea;                    /* PIXJ_PART_*: the reconstruction's progress (pix_prog.rows: superblock rows done);
	                                       PIXJ_HALF_*: this job's loop filter progress (rows filtered), the other job reads it */
	uint64_t peer_ea;                   /* PIXJ_HALF_*: the other job's loop filter progress */
	uint32_t seam, pad3;                /* PIXJ_HALF_*: the superblock column where the right half's tile columns start */
	pix_frame pred;                     /* PIXJ_RSX_PRED: the prediction planes, frame position 0,0 (luma aw x ah) */
	uint32_t pred_pitch_y, pred_pitch_c;
	uint64_t pred_lab_ea;               /* ... the label the RSX counts finished superblock rows in (0: all done) */
	uint32_t pred_lab_base;             /* ... its value once row 0 is done (row r: base + r, compared modulo 2^32) */
	uint64_t store_ea;                  /* PIXJ_SBSTORE: the store (128-byte aligned) */
	uint32_t store_rows, pad4;          /* ... its superblock rows */
	uint32_t rsx_sizes;                 /* PIXJ_RSX_PRED: bit b: the RSX predicts the blocks of size index b that have one
	                                       reference (rsxmc_geom.sizes); 0: all of them. The SPU predicts the others.
	                                       Bits 16 + b: the same for the blocks with two references when
	                                       PIXJ_RSX_COMPOUND is set (rsxmc_geom.sizes2); 0: as bits 0 - 12 */
} __attribute__((aligned(128))) pix_job;

/* A frame's progress, written by the job that decodes it whenever rows become final (loop filtered, borders filled):
   reference windows may read luma rows < lim_y and chroma rows < lim_c. PIX_LIM_NONE: nothing yet (the top border
   neither); PIX_LIM_ALL: the whole frame with its borders. The records come superblock row by row (every row tile by
   tile), so a frame's rows become final from the top while it is decoded, and the next frame can start before. */
typedef struct {
	uint32_t rows;                      /* superblock rows loop filtered */
	int32_t lim_y, lim_c;
	uint32_t pad;
} pix_prog;

#define PIX_LIM_NONE    ((int32_t)0x80000001)
#define PIX_LIM_ALL     0x7FFFFFFF

enum { PIX_OK = 0, PIX_BAD_JOB = 1, PIX_BAD_RECORDS = 2, PIX_RSX_TIMEOUT = 3 };   /* 3: an RSX row did not come (PIXJ_RSX_PRED) */

#define PIX_CSUM_ROWS 24                /* pix_result.csum_row / csum_col: 1440p has 23 superblock rows, 40 columns */
#define PIX_CSUM_COLS 40
#define PIX_WAIT_ROWS 24                /* pix_result.wait_rows: 1440p has 23 superblock rows */

typedef struct {
	uint32_t seq, status;
	/* SPU decrementer ticks (79.8 MHz on the PS3) */
	uint32_t ticks_total, ticks_pass1, ticks_pass2, ticks_pass3;
	uint32_t ticks_inter;               /* inter prediction (the convolutions) */
	uint32_t ticks_intra;               /* intra prediction */
	uint32_t ticks_resid;               /* coefficients and inverse transforms */
	uint32_t ticks_wait_ref;            /* waiting for reference windows */
	uint32_t ticks_wait_rec;            /* waiting for records */
	uint32_t ticks_wait_io;             /* waiting for superblock / loop filter / border DMA */
	uint32_t ticks_lfmask;              /* building the loop filter masks */
	uint32_t ticks_idle, ticks_answer;
	uint32_t blocks, ref_windows, slow_windows;
	uint32_t prefetched;                /* inter blocks whose windows came in while the block before was computed */
	/* PIXJ_SELFTEST */
	uint32_t st_conv, st_conv_bad;      /* convolution cases, cases different from libvpx's C */
	uint32_t st_lpf, st_lpf_bad;        /* loop filter cases (superblocks), cases different */
	uint32_t st_ticks[4];               /* convolution SIMD, C; loop filter SIMD, C */
	uint32_t st_first[3];               /* the first difference: what (see main.c), its parameters, where */
	uint32_t ticks_lf;                  /* pass 2 without its DMA waits: the filtering */
	uint32_t st_itx, st_itx_bad;        /* inverse transform cases, cases different */
	uint32_t st_ticks_itx[2];           /* inverse transforms SIMD, C */
	uint32_t lf_edges;                  /* loop filter: 16-lane edge runs filtered (two 8x8 blocks each) */
	uint32_t lf_f4, lf_f8, lf_f16;      /* ... in how many some lane took filter4 / filter8 / filter16 */
	uint32_t ticks_lf_tr;               /* ... transposing for the vertical edges */
	uint32_t st_bench[6];               /* self-test: ticks for PIX_ST_BENCH loop filter edge runs with no filter, filter4,
	                                       filter8, filter16, filter4 + 16; for PIX_ST_BENCH 16x16 block transposes */
	uint32_t ticks_plan;                /* pass 1: planning (and asking for) the windows of blocks that were not prefetched */
	uint32_t ticks_coef;                /* ... finding where a block's transform data end */
	uint32_t ticks_next;                /* ... the next block: its head, its windows planned and asked for */
	uint32_t st_lpf_bad2;               /* self-test: loop filter cases where version 2 differs from libvpx's C */
	uint32_t st_ticks_lf2;              /* ... version 2's ticks (version 1's: st_ticks[2]) */
	uint32_t pad2;
	uint32_t ticks_wait_prog;           /* waiting for reference frames' rows (other jobs still decoding them) */
	uint32_t csum[4];                   /* a sum over the frame's final pixels as they go out (the same decode, the same sum) */
	uint32_t st_ip, st_ip_bad;          /* self-test: intra prediction cases, cases different from libvpx's C */
	uint32_t st_ticks_ip[2];            /* ... SIMD, C */
	uint32_t ticks_lf_wait;             /* pass 2: waiting for its DMA (part of ticks_wait_io) */
	uint32_t ticks_wait_half;           /* a split frame: waiting for the other job (its reconstruction / its loop filter) */
	uint32_t st_lfm, st_lfm_bad;        /* self-test: superblocks' loop filter masks block by block against vp9_setup_mask */
	uint32_t st_ticks_lfm[2];           /* ... ticks: block by block, libvpx's */
	/* PIX_DETAIL_TIMERS (diagnosis builds): pass 1 split further */
	uint32_t ticks_head;                /* do_block's start: the head, a block not prefetched planned (ticks_plan), waits */
	uint32_t ticks_txwalk;              /* the transform blocks' loop without the intra prediction and residual in it */
	uint32_t ticks_mask;                /* the mode info and the loop filter masks of the block */
	uint32_t ticks_sbbegin;             /* sb_begin */
	uint32_t ticks_rowdone;             /* row_done in pass 1 (with the loop filter, borders and waits it runs) */
	uint32_t ticks_txloop;              /* pass 1: the transform blocks' loop of every block (intra prediction, residual and
	                                       the walk; ticks_intra / ticks_resid split it only with PIX_DETAIL_TIMERS) */
	uint32_t wait_rows[PIX_WAIT_ROWS];  /* a split frame's job: ticks waiting for the other half, per superblock row
	                                       (left half: before filtering row k; right half: before filtering row k; the last
	                                       entry also takes rows beyond it) */
	uint32_t ticks_wait_rsx;            /* PIXJ_RSX_PRED: waiting for the RSX's rows */
	uint32_t ticks_pred;                /* ... fetching the predictions (DMA wait and copy into the superblock buffer) */
	uint32_t rsx_blocks;                /* ... blocks predicted by the RSX */
	uint32_t rsx_chk_n, rsx_chk_bad;    /* PIXJ_RSX_CHECK: superblocks checked; those whose predictions changed after their fetch */
	uint32_t rsx_chk_first;             /* ... the first one: row << 16 | column */
	uint32_t rsx_chk_age;               /* ... its fetch: ticks after this SPU saw the row's label */
	uint32_t rsx_chk_waited;            /* ... and whether the SPU had waited for that label (1) or found it there (0) */
	uint32_t rsx_cmp_blocks, rsx_cmp_bad;   /* PIXJ_RSX_CHECK: blocks compared with the SPU's own prediction; those that differ */
	uint32_t rsx_cmp_bytes;             /* ... bytes that differ */
	uint32_t rsx_cmp_first;             /* ... the first block: mi_row << 16 | mi_col */
	uint32_t rsx_cmp_at;                /* ... its first byte: plane << 28 | y << 14 | x (the plane's pixels) */
	uint32_t rsx_cmp_vals;              /* ... block size << 16 | the RSX's byte << 8 | the SPU's */
	uint32_t rsx_cmp_age, rsx_cmp_waited;   /* ... its superblock's fetch: as rsx_chk_age, rsx_chk_waited */
	uint32_t rsx_cmp_again;             /* ... 1: the superblock's predictions read again right then equal the SPU's (the RSX's
	                                       pixels came after the fetch); 0: still different */
	uint32_t csum_row[PIX_CSUM_ROWS];   /* csum's parts per superblock row and per superblock column (the lanes folded
	                                       into one word): where two decodes of a frame differ */
	uint32_t csum_col[PIX_CSUM_COLS];
	uint32_t stack_free;                /* local store bytes between the program's end and the deepest stack use so far */
} __attribute__((aligned(128))) pix_result;

#define PIX_ST_BENCH 256                /* the self-test's timing runs per case */

#define PIX_ANSWER(seq, status)  ((((seq) & 0xFFFF) << 8) | ((status) & 0xFF))

#endif

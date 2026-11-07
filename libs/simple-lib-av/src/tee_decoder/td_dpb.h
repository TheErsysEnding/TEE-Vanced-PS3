/* TEE Decoder building block - made by tools/make_baustein.sh from common/dpb.h; do not edit here: change the
   project's file and run the script again. */
/* Decoded picture buffer for the reconstruction test: picture order count (8.2.1, types 0 and 2),
   reference picture lists with modification (8.2.4) and reference marking (8.2.5: sliding window and
   MMCO 1-6). Frames only. Used by the Mac tool and the PS3's PPU. */
#ifndef DPB_H
#define DPB_H

#include "td_h264_hdr.h"
#include "td_recon.h"

#define DPB_SLOTS 24

typedef struct {
	recon_pic pic;
	uint8_t *mem;                   /* the planes' allocation */
	int ref;                        /* 0 unused, 1 short-term, 2 long-term */
	int busy;                       /* holds (TEE Decoder: being decoded, referenced by a job in flight, waiting
	                                   for display or for the check); free when 0 and not a reference */
	int frame_num, frame_num_wrap, long_term_frame_idx;
	int pic_num, long_term_pic_num;
} dpb_slot;

typedef struct {
	dpb_slot s[DPB_SLOTS];
	int width, height;              /* coded size (multiple of 16) */
	int max_refs, max_long_term_idx;
	int prev_poc_msb, prev_poc_lsb, prev_frame_num, prev_frame_num_offset, prev_ref_had_mmco5;
	int cur;                        /* slot of the picture being decoded */
	int serial;                     /* pictures begun so far: the next picture's id */
	int nslots;                     /* slots allocated, at most DPB_SLOTS */
} dpb;

/* allocation: alloc(size) must return memory aligned to 128 bytes; NULL on failure. nslots picture buffers
   (0: max_refs + 4), at most DPB_SLOTS */
int  dpb_init(dpb *d, int width, int height, int max_refs, int nslots, void *(*alloc)(size_t));
/* the bytes one slot takes (planes with borders and the motion field) */
size_t dpb_slot_bytes(int width, int height);

/* begins a picture: POC, a free slot, the reference lists in *rp (num_ref, ref, cur_poc, weights left to
   the caller). Returns the slot or -1. */
int  dpb_begin(dpb *d, const h264_sps *sps, const h264_pps *pps, const h264_slice_hdr *sh, recon_params *rp);

/* after the picture is decoded: reference marking; the slot stays busy until dpb_release */
void dpb_end(dpb *d, const h264_sps *sps, const h264_slice_hdr *sh);
void dpb_hold(dpb *d, int slot);       /* one more hold */
void dpb_release(dpb *d, int slot);    /* one hold less */
/* TEE Decoder: every slot free and no reference, the memory kept (a new run of the same stream) */
void dpb_reset(dpb *d);
/* slots dpb_begin could take now (ignore_refs: an IDR picture, which unmarks every reference first) */
int  dpb_free_slots(const dpb *d, int ignore_refs);

#endif

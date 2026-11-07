// decode-h264 - H.264 decoder wrapper: the TEE Decoder or cellVdec behind one interface. See decode-h264.h.
//
// cellVdec runs asynchronously on its own PPU thread + 4 SPUs and reports progress through a
// callback: AUDONE (an access unit was consumed), PICOUT (a decoded picture is ready), ERROR. We
// feed access units and, per the sample, retrieve each picture with GetPicItem + GetPicture.
//
// Nothing here blocks. The decoder cannot consume the next AU until retrieved pictures free its
// internal frame buffers, so waiting for AUDONE without pulling pictures deadlocks (and times out).
// Instead decodeAuH264 just feeds (reporting BUSY back to the caller), isAuConsumedH264 tells the
// caller when the AU buffer may be reused, and the caller keeps pulling pictures in between.
//
// The TEE Decoder (tee_decoder/, TEE's own, vendored unchanged - never edit it here) runs its own PPU thread
// and 4 or 5 SPU thread groups. It copies every access unit it is fed, hands pictures out in display order with
// the pts they were fed with, and is copied out of here into the caller's frame slot (tdec_copy_i420), so
// nothing after this file changes. One TEE Decoder at a time: its platform layer is a singleton.
#include "decode-h264.h"
#include "tee_decoder/tee_decoder.h"
#include "thread.h"             // createLock / lock / sleepMs
#include "dbg.h"                // logInfo / logError
#include <cell/codec/vdec.h>
#include <cell/codec/vdec_avc.h>
#include <cell/sysmodule.h>
#include <sys/spu_initialize.h>
#include <sys/sys_time.h>       // sys_time_get_current_time (diagnostic: decoder own-latency split)
#include <sys/memory.h>         // sys_memory_get_user_memory_size (the TEE Decoder's memory check)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define PICOUT_TIME_RING 32     // power of two: masks the write/read counters

// microseconds on the SAME wall clock the caller stamps its feed time with (cell-stream's getTimeUs),
// so the two can be subtracted. must NOT use sys_time_get_system_time - that is a different epoch.
static uint64_t nowUs(void)
{
   sys_time_sec_t seconds;
   sys_time_nsec_t nanoseconds;
   sys_time_get_current_time(&seconds, &nanoseconds);
   return (uint64_t)seconds * 1000000ull + nanoseconds / 1000;
}

// cellVdec resource tuning (from the SDK pamf_dmux sample): AVC uses 4 SPUs.
#define VDEC_SPU_COUNT       4
#define VDEC_PPU_PRIORITY    720
#define VDEC_SPU_PRIORITY    200
#define VDEC_PPU_STACK_SIZE  (16 * 1024)

// The TEE Decoder, as TEE Vanced runs it: about 91 MB at 1080p instead of the default 112 MB - fewer picture
// buffers and a smaller input ring, but TEE's own 32 MB records ring. The 16 MB of the lean setting in TEE's
// guide played 1080p60 at 60 fps on the console (Test1, 2026-11-07), but YouTube's high-motion pictures
// (2.5-4.3 MB of CABAC records each) did not fit into what was free of it 33 times in 110 s and had their
// CABAC run again - headroom spent for nothing, with 56 MB of user memory still free. Records and input sizes
// stay powers of two, the records ring never below 8 MB (a ring smaller than one picture retries forever).
// The SPE time-out sits below video-player.c's 3 s stall detector, so a silent SPE becomes the decoder's own
// error (which ends its SPU groups) before the player flushes it.
#define TEE_PICTURES         12
#define TEE_INPUT_BYTES      (4u << 20)
#define TEE_RECORDS_HD       (32u << 20)   // above 720 rows
#define TEE_RECORDS_SD       (8u << 20)
#define TEE_TIMEOUT_MS       2000
#define TEE_FIXED_BYTES      (6u << 20)    // its slice ring and NAL buffer
#define TEE_BACKLOG_LIMIT    48             // AUs fed and not yet out as pictures (see getAuBacklogLimitH264)
#define TEE_STATS_US         5000000ull     // one statistics line this often while it plays

enum { BACKEND_VDEC = 1, BACKEND_TEE = 2 };

struct H264Decoder {
   int            backend;
   int            width, height;   // coded size the caller sized its frame buffer for
   int            catchUpSkip;     // may drop non-reference pictures while behind (see header)
   volatile int   errored;

   // cellVdec
   CellVdecHandle handle;
   void          *workMemory;
   sys_lwmutex_t  lock;
   int            lockCreated;

   int            auFedCount;     // AUs handed to cellVdecDecodeAu (decode thread only)
   volatile int   auDoneCount;     // incremented by the callback on each AUDONE
   volatile int   picPending;      // decoded pictures reported but not yet retrieved
   volatile int   seqDone;         // EndSeq finished (set by the callback, cleared by reset)
   int            auRejectedCount;   // AUs the decoder refused (a broken stream refuses every one)
   int            picIncompleteCount;   // pictures that came out damaged

   // diagnostic: when each PICOUT fired, so a caller can split its feed->retrieve time into the
   // decoder's own latency (feed->PICOUT) and the caller's drain lag (PICOUT->retrieve). one stamp
   // pushed per PICOUT, one consumed per retrieved picture - they stay paired (pictures come out in
   // feed order and every retrieval decrements picPending exactly once).
   uint64_t       picoutUs[PICOUT_TIME_RING];
   volatile int   picoutWrite;    // callback advances
   int            picoutRead;      // getFrameH264 advances
   uint64_t       lastReadyUs;     // PICOUT time of the most recently retrieved picture

   // TEE Decoder
   tdec          *tee;
   int            teeFed, teeOut;          // since open / the last flush: AUs taken, pictures handed out
   int            teeEnded, teeDrained;    // end of stream told / every picture out after it
   int            teeErrorLogged;
   uint64_t       statsSinceUs;            // the statistics window
   int            statsOut, statsRefused, statsDropped;
   uint64_t       statsCopyUs, statsCopyMaxUs;
};

// ============================================================================
// which backend
// ============================================================================

static H264Policy policy = H264_POLICY_AUTO;
static int teeSpes = 4;          // 4 = 1 CABAC + 3 pixel, 5 = 1 CABAC + 4 pixel
static int teeOff;               // the TEE Decoder failed while playing: cellVdec until the app restarts
static int teeOpen;              // one TEE Decoder at a time
static int spuInitialized;

void setH264DecoderPolicy(H264Policy newPolicy, int spes)
{
   policy = newPolicy == H264_POLICY_SONY ? H264_POLICY_SONY : H264_POLICY_AUTO;
   teeSpes = spes == 5 ? 5 : 4;
}

int isH264TeeUsable(void) { return policy == H264_POLICY_AUTO && !teeOff; }

void disableH264TeeForSession(const char *why)
{
   if (!teeOff) logWarn("[decode-h264] TEE Decoder off until the app restarts: %s\n", why ? why : "?");
   teeOff = 1;
}

// SPU runtime is process-global and the same for both backends: 6 SPUs at most, raw ones none. The TEE
// Decoder asks for the same itself (and is told so harmlessly); never more - a 7th froze TEE's console.
static void initSpusOnce(void)
{
   if (!spuInitialized) { sys_spu_initialize(6, 0); spuInitialized = 1; }
}

const char *getH264DecoderName(const H264Decoder *decoder)
{
   if (!decoder) return "none";
   return decoder->backend == BACKEND_TEE ? "TEE Decoder" : "cellVdec";
}

int isTeeH264Decoder(const H264Decoder *decoder) { return decoder && decoder->backend == BACKEND_TEE; }

// ============================================================================
// cellVdec
// ============================================================================

static uint32_t vdecCallback(CellVdecHandle handle, CellVdecMsgType type, int32_t data, void *arg)
{
   (void)handle;
   H264Decoder *decoder = (H264Decoder *)arg;
   lock(&decoder->lock);
   switch (type) {
      // a rejected access unit is reported here, NOT as MSG_TYPE_ERROR (which is fatal-only): the
      // decoder still emits a picture for it, so without this line a broken stream looks healthy.
      // throttled: a broken stream rejects every AU, which would be 30-60 lines a second
      case CELL_VDEC_MSG_TYPE_AUDONE:
         decoder->auDoneCount++;
         if (data != CELL_OK) {
            decoder->auRejectedCount++;
            if (decoder->auRejectedCount == 1 || decoder->auRejectedCount % 256 == 0)
               logWarn("[decode-h264] decoder rejected access unit #%d, rc=0x%x\n", decoder->auRejectedCount, data);
         }
         break;
      case CELL_VDEC_MSG_TYPE_PICOUT:
         decoder->picoutUs[decoder->picoutWrite & (PICOUT_TIME_RING - 1)] = nowUs();
         decoder->picoutWrite++;
         decoder->picPending++;
         break;
      case CELL_VDEC_MSG_TYPE_SEQDONE: decoder->seqDone = 1;  break;
      case CELL_VDEC_MSG_TYPE_ERROR:
         decoder->errored = 1;
         logError("[decode-h264] vdec fatal error 0x%x\n", data);
         break;
   }
   unlock(&decoder->lock);
   return 0;
}

// asks cellVdec (via the Ex query) for `buffers` decoded-frame buffers at this level/size; returns the
// required work-memory size, or 0 if the decoder rejects that buffer count.
static size_t queryWorkMemory(int width, int height, int level, int buffers)
{
   CellVdecAvcSpecificInfo avcInfo = { sizeof avcInfo, (uint16_t)width, (uint16_t)height, false, (uint8_t)buffers };
   CellVdecTypeEx type = { CELL_VDEC_CODEC_TYPE_AVC, (uint32_t)level, &avcInfo };
   CellVdecAttr attr;
   return cellVdecQueryAttrEx(&type, &attr) == CELL_OK ? attr.memSize : 0;
}

H264Decoder *createH264Decoder(int width, int height, int level, int maxRefFrames)
{
   H264Decoder *decoder = (H264Decoder *)calloc(1, sizeof *decoder);
   if (!decoder) return 0;
   decoder->backend = BACKEND_VDEC;
   decoder->width  = width;
   decoder->height = height;
   createLock(&decoder->lock);
   decoder->lockCreated = 1;

   // SPU runtime + codec module are process-global: init the SPUs once, and tolerate a duplicate
   // module load if another decoder already brought it up.
   initSpusOnce();
   int ret = cellSysmoduleLoadModule(CELL_SYSMODULE_VDEC_AVC);
   if (ret != CELL_OK && ret != CELL_SYSMODULE_ERROR_DUPLICATED) {
      logError("[decode-h264] load VDEC_AVC failed 0x%x\n", ret);
      goto fail;
   }

   // the decoded-picture buffer must hold every reference frame plus the one being decoded. Ask for
   // refs+1 and, if the decoder rejects it, step down to the largest count it accepts.
   int wantBuffers = maxRefFrames > 0 ? maxRefFrames + 1 : 4;
   size_t memSize = 0; int gotBuffers = 0;
   for (int buffers = wantBuffers; buffers >= 2 && memSize == 0; buffers--) {
      memSize = queryWorkMemory(width, height, level, buffers);
      if (memSize) gotBuffers = buffers;
   }
   if (!memSize) { logError("[decode-h264] no acceptable DPB size at %dx%d level %d\n", width, height, level); goto fail; }

   decoder->workMemory = malloc(memSize);
   if (!decoder->workMemory) goto fail;

   CellVdecAvcSpecificInfo avcInfo = { sizeof avcInfo, (uint16_t)width, (uint16_t)height, false, (uint8_t)gotBuffers };
   CellVdecTypeEx type = { CELL_VDEC_CODEC_TYPE_AVC, (uint32_t)level, &avcInfo };

   CellVdecResourceEx resource;
   resource.memAddr            = decoder->workMemory;
   resource.memSize            = memSize;
   resource.ppuThreadPriority  = VDEC_PPU_PRIORITY;
   resource.ppuThreadStackSize = VDEC_PPU_STACK_SIZE;
   resource.spuThreadPriority  = VDEC_SPU_PRIORITY;
   resource.numOfSpus          = VDEC_SPU_COUNT;
   resource.spursResource      = 0;   // NULL: libvdec builds its own SPURS instance of numOfSpus SPUs

   CellVdecCb callback = { vdecCallback, decoder };
   ret = cellVdecOpenEx(&type, &resource, &callback, &decoder->handle);
   if (ret != CELL_OK) { logError("[decode-h264] OpenEx failed 0x%x\n", ret); goto fail; }

   ret = cellVdecStartSeq(decoder->handle);
   if (ret != CELL_OK) { logError("[decode-h264] StartSeq failed 0x%x\n", ret); cellVdecClose(decoder->handle); decoder->handle = 0; goto fail; }

   logInfo("[decode-h264] cellVdec opened %dx%d level %d, %d DPB buffers, workMem %d KB\n", width, height, level, gotBuffers, (int)(memSize / 1024));
   return decoder;

fail:
   destroyH264Decoder(decoder);
   return 0;
}

static int vdecDecodeAu(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   if (decoder->errored) return -1;

   uint64_t ticks90kHz = pts / 100000 * 9 + pts % 100000 * 9 / 100000;   // ns -> 90 kHz, no overflow

   CellVdecAuInfo au;
   au.startAddr         = (void *)data;
   au.size              = size;
   au.pts.upper         = (uint32_t)(ticks90kHz >> 32) & 1;   // the decoder expects 33-bit 90 kHz stamps
   au.pts.lower         = (uint32_t)ticks90kHz;
   au.dts.upper         = CELL_VDEC_DTS_INVALID;
   au.dts.lower         = CELL_VDEC_DTS_INVALID;
   au.userData          = pts;                                // full-resolution pts, echoed on the pic item
   au.codecSpecificData = 0;

   // Dropping this AU's non-reference B pictures is the only lever cellVdec offers to catch up.
   // Whether we are actually behind is not knowable here - the AU backlog is a pipeline depth, not
   // a lag measure (the decode thread deliberately keeps the queue full for throughput) - so the
   // player decides, against the clock the picture is really paced to, and just sets the flag.
   int ret = cellVdecDecodeAu(decoder->handle,
                              decoder->catchUpSkip ? CELL_VDEC_DEC_MODE_B_SKIP
                                                   : CELL_VDEC_DEC_MODE_NORMAL, &au);
   if (ret == CELL_OK) { decoder->auFedCount++; return 0; }
   if (ret == (int)CELL_VDEC_ERROR_BUSY) return 1;   // queues full: pull pictures, then retry this AU
   logError("[decode-h264] DecodeAu failed 0x%x\n", ret);
   return -1;
}

// releases decoded pictures the callback has reported, one GetPicItem + GetPicture(NULL discard)
// pair per PICOUT — the SDK samples never retrieve speculatively, only in response to a PICOUT
static void discardReportedPictures(H264Decoder *decoder, const CellVdecPicFormat *format)
{
   while (decoder->picPending > 0) {
      lock(&decoder->lock); decoder->picPending--; unlock(&decoder->lock);
      const CellVdecPicItem *picItem;
      if (cellVdecGetPicItem(decoder->handle, &picItem) == CELL_OK) cellVdecGetPicture(decoder->handle, format, 0);
   }
}

static int vdecReset(H264Decoder *decoder)
{
   // flush for a seek per the SDK protocol (pamf_dmux_trick_play, libavdecode): EndSeq retried while
   // the input queue is full, every flushed picture released as its PICOUT arrives until SEQDONE
   // (one last drain after it — a PICOUT can land between our final check and SEQDONE, and a picture
   // left unreleased starves the refs+1-sized DPB of the next sequence), then StartSeq on the same
   // handle, retried — Sony's own middleware notes StartSeq "sometimes fails with 'fatal' errors
   // that aren't fatal" right after a flush and brute-forces it.
   decoder->seqDone = 0;

   int ret;
   for (int tries = 0; (ret = cellVdecEndSeq(decoder->handle)) == (int)CELL_VDEC_ERROR_BUSY && tries < 1000; tries++) sleepMs(1);
   if (ret != CELL_OK) { logError("[decode-h264] EndSeq failed 0x%x\n", ret); return -1; }

   CellVdecPicFormat format;
   format.formatType      = CELL_VDEC_PICFMT_YUV420_PLANAR;
   format.colorMatrixType = CELL_VDEC_COLOR_MATRIX_TYPE_BT709;
   format.alpha           = 0xFF;

   for (int waited = 0; !decoder->seqDone && !decoder->errored && waited < 2000; waited++) {
      discardReportedPictures(decoder, &format);
      sleepMs(1);
   }
   discardReportedPictures(decoder, &format);

   if (!decoder->seqDone) {
      logError("[decode-h264] EndSeq never completed%s\n", decoder->errored ? " (decoder errored)" : "");
      return -1;
   }

   decoder->auFedCount = 0;
   decoder->auDoneCount = 0;
   decoder->picPending = 0;

   for (int tries = 0; (ret = cellVdecStartSeq(decoder->handle)) != CELL_OK && tries < 512; tries++) sleepMs(1);
   if (ret != CELL_OK) { logError("[decode-h264] StartSeq (reset) failed 0x%x\n", ret); return -1; }
   return 0;
}

static int vdecGetFrame(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts)
{
   if (decoder->errored) return -1;
   if (decoder->picPending <= 0) return 0;

   const CellVdecPicItem *picItem;
   if (cellVdecGetPicItem(decoder->handle, &picItem) != CELL_OK) return 0;   // counter ran ahead: nothing queued yet
   lock(&decoder->lock);
   decoder->picPending--;
   decoder->lastReadyUs = decoder->picoutUs[decoder->picoutRead & (PICOUT_TIME_RING - 1)];
   decoder->picoutRead++;
   unlock(&decoder->lock);

   // throttled: a degraded stream damages every picture, which would be 30-60 lines a second
   if (picItem->status != CELL_OK) {
      decoder->picIncompleteCount++;
      if (decoder->picIncompleteCount == 1 || decoder->picIncompleteCount % 256 == 0)
         logWarn("[decode-h264] picture #%d came out incomplete, status=0x%x\n", decoder->picIncompleteCount, picItem->status);
   }

   const CellVdecAvcInfo *info = (const CellVdecAvcInfo *)picItem->picInfo;
   int frameW = info->horizontalSize;
   int frameH = info->verticalSize;
   *outPts = picItem->auUserData[0];   // the ns pts fed with the AU; auPts is the truncated 90 kHz stamp

   // YUV planar output: the decoder skips its (SPU-costly) colorspace conversion and hands back the
   // native planes; the RSX converts to RGB in a fragment shader at draw time
   CellVdecPicFormat format;
   format.formatType     = CELL_VDEC_PICFMT_YUV420_PLANAR;
   format.colorMatrixType = frameH >= 720 ? CELL_VDEC_COLOR_MATRIX_TYPE_BT709 : CELL_VDEC_COLOR_MATRIX_TYPE_BT601;
   format.alpha          = 0xFF;

   // a skipped picture (B_SKIP mode) has no data but must still be released with a NULL GetPicture,
   // or the decoder's output queue clogs and it stops consuming input (per the SDK vpost sample)
   if (picItem->attr == CELL_VDEC_PICITEM_ATTR_SKIPPED) {
      cellVdecGetPicture(decoder->handle, &format, 0);
      return 0;
   }

   int ret = cellVdecGetPicture(decoder->handle, &format, yuvOut);
   if (ret != CELL_OK) { logError("[decode-h264] GetPicture failed 0x%x\n", ret); return -1; }

   *outWidth  = frameW;
   *outHeight = frameH;
   return 1;
}

static void vdecDestroy(H264Decoder *decoder)
{
   if (decoder->handle) { cellVdecEndSeq(decoder->handle); cellVdecClose(decoder->handle); }
   free(decoder->workMemory);
   if (decoder->lockCreated) destroyLock(&decoder->lock);
}

// ============================================================================
// TEE Decoder: can it decode this stream?
// ============================================================================

// What the TEE Decoder refuses, or would decode wrong without saying so (TEE's td_core.c / td_h264_hdr.c): it
// takes High and Main profile, CABAC, 8 bit 4:2:0, progressive frames, one slice group, picture order count
// type 0 or 2, no frame_num gaps, no scaling matrices, no constrained intra prediction, no redundant pictures,
// a coded width that is a multiple of 128 up to 1920 x 1088. Read here from the stream's own SPS and PPS, so a
// stream it cannot take never reaches it. Several slices per picture only shows while it plays: the decoder
// stops with an error then, and video-player.c falls back to cellVdec.

typedef struct { uint8_t data[256]; int size; int bit; int bad; } Rbsp;

// the RBSP of one NAL unit (after its header byte): emulation prevention bytes removed
static void loadRbsp(Rbsp *r, const uint8_t *nal, int size)
{
   r->size = 0; r->bit = 0; r->bad = 0;
   int zeros = 0;
   for (int i = 0; i < size; i++) {
      if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
      zeros = nal[i] == 0 ? zeros + 1 : 0;
      if (r->size >= (int)sizeof r->data) { r->bad = 1; return; }
      r->data[r->size++] = nal[i];
   }
}

static uint32_t readBit(Rbsp *r)
{
   if (r->bit >= r->size * 8) { r->bad = 1; return 0; }
   uint32_t value = (r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
   r->bit++;
   return value;
}

static uint32_t readBits(Rbsp *r, int count)
{
   uint32_t value = 0;
   while (count-- > 0) value = (value << 1) | readBit(r);
   return value;
}

static uint32_t readUe(Rbsp *r)
{
   int zeros = 0;
   while (!r->bad && readBit(r) == 0) if (++zeros > 31) { r->bad = 1; return 0; }
   return ((1u << zeros) - 1) + readBits(r, zeros);
}

static int32_t readSe(Rbsp *r)
{
   uint32_t code = readUe(r);
   return (code & 1) ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
}

// more_rbsp_data(): is there anything before the RBSP stop bit?
static int moreRbspData(const Rbsp *r)
{
   int last = r->size - 1;
   while (last >= 0 && r->data[last] == 0) last--;
   if (last < 0) return 0;
   int low = 0;
   while (!((r->data[last] >> low) & 1)) low++;
   return r->bit < last * 8 + (7 - low);
}

static int teeTakesSps(Rbsp *r, char *why, int cap)
{
   int profile = (int)readBits(r, 8);
   readBits(r, 16);                        // constraint flags, level
   readUe(r);                              // seq_parameter_set_id
   if (profile != 77 && profile != 100) { snprintf(why, cap, "profile %d (it takes Main and High)", profile); return 0; }
   if (profile == 100) {
      uint32_t chroma = readUe(r);
      if (chroma != 1) { snprintf(why, cap, "chroma format %u (it takes 4:2:0)", (unsigned)chroma); return 0; }
      uint32_t lumaDepth = readUe(r), chromaDepth = readUe(r);
      if (lumaDepth || chromaDepth) { snprintf(why, cap, "%u bit (it takes 8 bit)", (unsigned)(8 + lumaDepth)); return 0; }
      readBit(r);                          // qpprime_y_zero_transform_bypass
      if (readBit(r)) { snprintf(why, cap, "scaling matrices in the SPS"); return 0; }
   }
   readUe(r);                              // log2_max_frame_num_minus4
   uint32_t pocType = readUe(r);
   if (pocType == 0) readUe(r);
   else if (pocType != 2) { snprintf(why, cap, "picture order count type %u", (unsigned)pocType); return 0; }
   readUe(r);                              // max_num_ref_frames
   if (readBit(r)) { snprintf(why, cap, "gaps in frame_num allowed"); return 0; }
   readUe(r); readUe(r);                   // size in macroblocks (the coded size is checked by the caller)
   if (!readBit(r)) { snprintf(why, cap, "interlaced (frame_mbs_only_flag 0)"); return 0; }
   if (r->bad) { snprintf(why, cap, "an SPS it could not read"); return 0; }
   return 1;
}

static int teeTakesPps(Rbsp *r, char *why, int cap)
{
   readUe(r); readUe(r);                   // pic / seq parameter set ids
   if (!readBit(r)) { snprintf(why, cap, "CAVLC (it takes CABAC)"); return 0; }
   readBit(r);                             // bottom_field_pic_order_in_frame_present
   if (readUe(r) != 0) { snprintf(why, cap, "slice groups"); return 0; }
   readUe(r); readUe(r);                   // num_ref_idx_l0/l1_default_active_minus1
   readBit(r); readBits(r, 2);             // weighted prediction
   readSe(r); readSe(r); readSe(r);        // pic_init_qp/qs, chroma_qp_index_offset
   readBit(r);                             // deblocking_filter_control_present
   if (readBit(r)) { snprintf(why, cap, "constrained intra prediction"); return 0; }
   if (readBit(r)) { snprintf(why, cap, "redundant pictures"); return 0; }
   if (moreRbspData(r)) {
      readBit(r);                          // transform_8x8_mode
      if (readBit(r)) { snprintf(why, cap, "scaling matrices in the PPS"); return 0; }
   }
   if (r->bad) { snprintf(why, cap, "a PPS it could not read"); return 0; }
   return 1;
}

// every SPS and PPS in an Annex-B buffer
static int teeTakesParameterSets(const uint8_t *data, int size, char *why, int cap)
{
   int spsCount = 0, ppsCount = 0;
   for (int i = 0; i + 3 < size; ) {
      if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) { i++; continue; }
      int start = i + 3, end = start;
      while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0 &&
                                 (data[end + 2] == 1 || (data[end + 2] == 0 && end + 3 < size && data[end + 3] == 1))))
         end++;
      if (end + 2 >= size) end = size;
      if (end > start) {
         int type = data[start] & 0x1F;
         if (type == 7 || type == 8) {
            Rbsp rbsp;
            loadRbsp(&rbsp, data + start + 1, end - start - 1);
            if (rbsp.bad) { snprintf(why, cap, "a parameter set longer than %d bytes", (int)sizeof rbsp.data); return 0; }
            if (type == 7) { spsCount++; if (!teeTakesSps(&rbsp, why, cap)) return 0; }
            else           { ppsCount++; if (!teeTakesPps(&rbsp, why, cap)) return 0; }
         }
      }
      i = end;
   }
   if (!spsCount || !ppsCount) { snprintf(why, cap, "no SPS or PPS in the stream header"); return 0; }
   return 1;
}

// the first slice of an access unit has nal_ref_idc 0: no other picture refers to it, so catch-up may drop it
static int isDisposableAu(const uint8_t *au, int size)
{
   for (int i = 0; i + 3 < size; i++) {
      if (au[i] != 0 || au[i + 1] != 0 || au[i + 2] != 1) continue;
      uint8_t header = au[i + 3];
      int type = header & 0x1F;
      if (type == 5) return 0;
      if (type == 1) return ((header >> 5) & 3) == 0;
      i += 3;
   }
   return 0;
}

// ============================================================================
// TEE Decoder
// ============================================================================

// its log lines (from its own thread, the SPU event thread or the caller's) - rate-limited, it can repeat one
// per picture on a stream it struggles with, and every logInfo is a file write. The line has no newline of its
// own and is never used as a format.
static void teeLog(const char *line)
{
   static uint64_t windowUs;
   static int lines, dropped;
   uint64_t now = sys_time_get_system_time();
   if (now - windowUs > 1000000ull) {
      if (dropped) logWarn("[tdec] %d log lines dropped\n", dropped);
      windowUs = now; lines = 0; dropped = 0;
   }
   if (++lines > 20) { dropped++; return; }
   logInfo("[tdec] %s\n", line);
}

static void logUserMemory(const char *when, unsigned needMb)
{
   sys_memory_info_t info;
   if (sys_memory_get_user_memory_size(&info) != CELL_OK) return;
   if (needMb) logInfo("[decode-h264] user memory %s: %u MB free of %u MB, the TEE Decoder needs about %u MB\n", when,
                       (unsigned)(info.available_user_memory >> 20), (unsigned)(info.total_user_memory >> 20), needMb);
   else        logInfo("[decode-h264] user memory %s: %u MB free of %u MB\n", when,
                       (unsigned)(info.available_user_memory >> 20), (unsigned)(info.total_user_memory >> 20));
}

static H264Decoder *createTee(const H264StreamParams *params, char *why, int cap)
{
   if (teeOpen) { snprintf(why, cap, "another TEE Decoder is still open"); return 0; }
   if (params->spsPatched) { snprintf(why, cap, "the SPS's reference count was rewritten for cellVdec"); return 0; }
   if (params->width % 128 || params->width > 1920 || params->height > 1088 || params->height < 32) {
      snprintf(why, cap, "coded size %dx%d (it takes widths that are a multiple of 128, up to 1920 x 1088)",
               params->width, params->height);
      return 0;
   }
   if (!teeTakesParameterSets(params->header, params->headerSize, why, cap)) return 0;

   tdec_config config;
   tdec_default(&config);
   config.max_width     = params->width;
   config.max_height    = params->height;
   config.pictures      = TEE_PICTURES;
   config.input_bytes   = TEE_INPUT_BYTES;
   config.records_bytes = params->height > 720 ? TEE_RECORDS_HD : TEE_RECORDS_SD;
   config.timeout_ms    = TEE_TIMEOUT_MS;
   config.log           = teeLog;
   if (teeSpes == 5) { config.ncabac = 1; config.npixel = 4; }   // tier 1 otherwise: 1 CABAC + 3 pixel

   // All of it is allocated up front. The kernel's free count is logged but decides nothing: memory a closed
   // decoder gave back stays in the malloc heap, so the kernel reported 38 MB free on the second video of Test4
   // (2026-11-07) while the heap still held the 90 MB the first one had used - and a check against that number
   // turned the TEE Decoder down and left 1080p60 to cellVdec. Whether the memory is there is what malloc says:
   // tdec_open fails cleanly when it is not, and the caller falls back to cellVdec then.
   size_t need = tdec_picture_memory_size(&config) + config.records_bytes + config.input_bytes + TEE_FIXED_BYTES;
   sys_memory_info_t info;
   if (sys_memory_get_user_memory_size(&info) == CELL_OK)
      logInfo("[decode-h264] user memory before the TEE Decoder: %u MB free of %u MB (heap not counted), it needs about %u MB\n",
              (unsigned)(info.available_user_memory >> 20), (unsigned)(info.total_user_memory >> 20), (unsigned)(need >> 20));

   H264Decoder *decoder = (H264Decoder *)calloc(1, sizeof *decoder);
   if (!decoder) { snprintf(why, cap, "out of memory"); return 0; }
   decoder->backend = BACKEND_TEE;
   decoder->width   = params->width;
   decoder->height  = params->height;

   initSpusOnce();
   char error[200];
   error[0] = 0;
   decoder->tee = tdec_open(&config, error, sizeof error);
   if (!decoder->tee) {
      snprintf(why, cap, "tdec_open: %s", error[0] ? error : "failed");
      free(decoder);
      return 0;
   }
   teeOpen = 1;
   decoder->statsSinceUs = sys_time_get_system_time();
   logInfo("[decode-h264] TEE Decoder %s opened for %dx%d: %d SPEs (1 CABAC + %d pixel), %d picture buffers\n",
           TEE_DECODER_VERSION, params->width, params->height, teeSpes, teeSpes - 1, TEE_PICTURES);
   logUserMemory("with the TEE Decoder open", 0);
   return decoder;
}

static void teeStats(H264Decoder *decoder, uint64_t now)
{
   if (now - decoder->statsSinceUs < TEE_STATS_US) return;
   int seconds = (int)((now - decoder->statsSinceUs) / 1000000ull);
   if (seconds < 1) seconds = 1;
   logInfo("[tdec] %d s: %d pictures out (%d/s), %d decoded since open or seek, %d feeds refused (input full), "
           "%d dropped by catch-up, copy %d us average, %d us max\n", seconds, decoder->statsOut, decoder->statsOut / seconds,
           tdec_pictures_decoded(decoder->tee), decoder->statsRefused, decoder->statsDropped,
           decoder->statsOut ? (int)(decoder->statsCopyUs / (uint64_t)decoder->statsOut) : 0, (int)decoder->statsCopyMaxUs);
   decoder->statsSinceUs = now;
   decoder->statsOut = decoder->statsRefused = decoder->statsDropped = 0;
   decoder->statsCopyUs = decoder->statsCopyMaxUs = 0;
}

static int teeFailed(H264Decoder *decoder)
{
   const char *error = tdec_error(decoder->tee);
   if (!error) return 0;
   if (!decoder->teeErrorLogged) { logError("[decode-h264] TEE Decoder stopped: %s\n", error); decoder->teeErrorLogged = 1; }
   decoder->errored = 1;
   return 1;
}

static int teeDecodeAu(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   if (decoder->errored || teeFailed(decoder)) return -1;
   if (decoder->catchUpSkip && isDisposableAu(data, size)) { decoder->statsDropped++; return 0; }
   int taken = tdec_feed(decoder->tee, data, (size_t)size, TDEC_ANNEXB, (int64_t)pts);
   if (taken == 1) { decoder->teeFed++; return 0; }
   if (taken == 0) {
      if (teeFailed(decoder)) return -1;   // after an error it reports "full" for ever
      decoder->statsRefused++;
      return 1;
   }
   logError("[decode-h264] the TEE Decoder refused an access unit of %d bytes (more than 2 MB?)\n", size);
   decoder->errored = 1;
   return -1;
}

static int teeGetFrame(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts)
{
   if (decoder->errored) return -1;
   uint64_t now = sys_time_get_system_time();
   tdec_picture picture;
   int got = tdec_get_picture(decoder->tee, &picture);
   if (got == 0) { teeStats(decoder, now); return 0; }
   if (got < 0) {
      if (teeFailed(decoder)) return -1;
      decoder->teeDrained = 1;             // after end of stream: every picture is out
      return 0;
   }
   if (picture.width > decoder->width || picture.height > decoder->height) {
      logError("[decode-h264] the TEE Decoder put out %dx%d, more than the %dx%d it was opened for\n",
               picture.width, picture.height, decoder->width, decoder->height);
      tdec_release_picture(decoder->tee, &picture);
      decoder->errored = 1;
      return -1;
   }
   tdec_copy_i420(&picture, (uint8_t *)yuvOut);
   uint64_t copyUs = sys_time_get_system_time() - now;
   tdec_release_picture(decoder->tee, &picture);

   decoder->teeOut++;
   decoder->statsOut++;
   decoder->statsCopyUs += copyUs;
   if (copyUs > decoder->statsCopyMaxUs) decoder->statsCopyMaxUs = copyUs;
   teeStats(decoder, now);

   *outWidth  = picture.width;
   *outHeight = picture.height;
   *outPts    = (uint64_t)picture.pts;
   return 1;
}

static int teeReset(H264Decoder *decoder)
{
   int ok = tdec_flush(decoder->tee) == 0;
   decoder->teeFed = decoder->teeOut = 0;
   decoder->teeEnded = decoder->teeDrained = 0;
   if (!ok) {
      const char *error = tdec_error(decoder->tee);
      logError("[decode-h264] TEE Decoder flush failed: %s\n", error ? error : "?");
      decoder->errored = 1;
      return -1;
   }
   decoder->errored = 0;
   decoder->teeErrorLogged = 0;
   return 0;
}

// ============================================================================
// the interface
// ============================================================================

H264Decoder *createH264DecoderFor(const H264StreamParams *params)
{
   if (isH264TeeUsable()) {
      char why[200];
      why[0] = 0;
      H264Decoder *decoder = createTee(params, why, sizeof why);
      if (decoder) return decoder;
      logInfo("[decode-h264] TEE Decoder not used: %s - cellVdec instead\n", why);
   } else {
      logInfo("[decode-h264] cellVdec: %s\n", policy == H264_POLICY_SONY ? "video-decoder=sony or a live stream"
                                                                         : "the TEE Decoder is off for this run");
   }
   return createH264Decoder(params->width, params->height, params->level, params->maxRefFrames);
}

int decodeAuH264(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   return decoder->backend == BACKEND_TEE ? teeDecodeAu(decoder, data, size, pts) : vdecDecodeAu(decoder, data, size, pts);
}

int getAuBacklogH264(const H264Decoder *decoder)
{
   if (decoder->backend == BACKEND_TEE) return decoder->teeFed - decoder->teeOut;
   return decoder->auFedCount - decoder->auDoneCount;
}

int getAuBacklogLimitH264(const H264Decoder *decoder, int auBufferCount)
{
   return decoder->backend == BACKEND_TEE ? TEE_BACKLOG_LIMIT : auBufferCount;
}

void setH264CatchUpSkip(H264Decoder *decoder, int enable)
{
   if (decoder) decoder->catchUpSkip = enable ? 1 : 0;
}

void endOfStreamH264(H264Decoder *decoder)
{
   if (!decoder || decoder->backend != BACKEND_TEE || decoder->teeEnded) return;
   tdec_end_of_stream(decoder->tee);
   decoder->teeEnded = 1;
}

int isH264Drained(const H264Decoder *decoder)
{
   return decoder && decoder->backend == BACKEND_TEE && decoder->teeDrained;
}

int resetH264Decoder(H264Decoder *decoder)
{
   return decoder->backend == BACKEND_TEE ? teeReset(decoder) : vdecReset(decoder);
}

int getFrameH264(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts)
{
   return decoder->backend == BACKEND_TEE ? teeGetFrame(decoder, yuvOut, outWidth, outHeight, outPts)
                                          : vdecGetFrame(decoder, yuvOut, outWidth, outHeight, outPts);
}

uint64_t getDecoderReadyUs(const H264Decoder *decoder) { return decoder->lastReadyUs; }

#define TEE_CLOSE_WAIT_MS 5000

static volatile int teeCloseDone;

static void teeCloseThread(uint64_t arg)
{
   tdec_close((tdec *)(uintptr_t)arg);
   __sync_synchronize();
   teeCloseDone = 1;
   exitThread();
}

// tdec_close on a thread of its own, waited for TEE_CLOSE_WAIT_MS: the module can hang in it - an SPE that never
// stops, its known issue after a pixel-SPE error (see the notes for the Mac master) - and the caller is the UI
// thread, so a hang froze the whole app (Test9, a 1080p60 live stream fed a cut segment). 1 once closed, 0 when it
// did not come back.
static int closeTeeBounded(tdec *tee)
{
   sys_ppu_thread_t thread;
   teeCloseDone = 0;
   if (spawnJoinableThread(&thread, teeCloseThread, (uint64_t)(uintptr_t)tee, THREAD_PRIORITY_DEFAULT,
                           THREAD_STACK_SIZE_64KB, "tdec-close") != 0) {
      tdec_close(tee);   // no thread for it: close it here, as before
      return 1;
   }
   for (int waited = 0; !teeCloseDone && waited < TEE_CLOSE_WAIT_MS; waited += 10) sleepMs(10);
   if (!teeCloseDone) return 0;
   joinThread(thread);
   return 1;
}

void destroyH264Decoder(H264Decoder *decoder)
{
   if (!decoder) return;
   if (decoder->backend == BACKEND_TEE) {
      if (decoder->tee) {
         if (!closeTeeBounded(decoder->tee)) {
            // left to the thread closing it, with its memory and its SPEs: freed it would be written into. The
            // TEE Decoder stays off - and counted open - for the rest of the run; the app goes on.
            logError("[decode-h264] the TEE Decoder did not close in %d ms (an SPE that never stops): left open\n",
                     TEE_CLOSE_WAIT_MS);
            disableH264TeeForSession("it did not close");
            return;
         }
         teeOpen = 0;
      }
      logUserMemory("after the TEE Decoder closed", 0);
   } else {
      vdecDestroy(decoder);
   }
   free(decoder);
}

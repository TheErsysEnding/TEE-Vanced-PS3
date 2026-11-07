#pragma once

// decode-h264 - H.264 access units in, YUV 4:2:0 planar pictures out (Y then U then V, packed). Two backends
// behind one interface:
//   - the TEE Decoder (TEE's own decoder on the SPEs, libs/simple-lib-av/src/tee_decoder/, vendored unchanged):
//     1080p60 at 60 fps with 4 SPEs. Used where it can decode the stream (see createH264DecoderFor).
//   - Sony's cellVdec (run across 4 SPUs): about 35 fps at 1080p. The fallback for everything else.
// Colour conversion happens on the RSX at draw time - asking cellVdec for RGB makes it convert on the same SPUs
// that decode, which costs real throughput.
//
// SPEs: one backend at a time. Either takes 4 of the 6; cellAdec (decode-aac.c) takes 1 more for the sound, so
// the TEE Decoder may use 5 at most (1 CABAC + 4 pixel), never its tier 2 (6) - TEE's rule: never more than 6 in
// all, a 7th froze the console.

#include <stdint.h>

typedef struct H264Decoder H264Decoder;

// IMPORTANT: width/height must be the stream's CODED size (what the SPS actually codes), not its
// display size. H.264 codes whole 16x16 blocks and hardware encoders pad further - Intel QuickSync
// codes 1280x720 as 1280x736 and crops it back for display. Get this wrong in either direction and
// cellVdec fails hard: told a size SMALLER than the stream it decodes nothing at all and hands the
// picture buffer back untouched (no error, black screen); told a size LARGER it locks the console.
// Callers must read the coded size from the stream (demuxers do; a network sender must transmit it).
//
// brings up a cellVdec decoder for a stream of the given coded size and AVC level (AVCLevelIndication).
// maxRefFrames is the SPS reference-frame count; the decoder asks cellVdec for that many decoded-frame
// buffers (+1) so high-reference streams don't overflow the level's default pool. NULL on failure.
H264Decoder *createH264Decoder(int width, int height, int level, int maxRefFrames);

// Which backend createH264DecoderFor may pick. Process-wide; the app sets it before it builds a player.
typedef enum {
   H264_POLICY_AUTO = 0,   // the TEE Decoder where it can decode the stream, cellVdec otherwise
   H264_POLICY_SONY = 1,   // cellVdec only (settings.txt video-decoder=sony; live streams)
} H264Policy;

// teeSpes: 4 = 1 CABAC + 3 pixel SPEs (TEE's tier 1, measured, the default), 5 = 1 CABAC + 4 pixel (faster at
// YouTube's bitrates, where the pixel stage is the limit; with the sound all 6 SPEs are busy). Anything else is 4.
void setH264DecoderPolicy(H264Policy policy, int teeSpes);

// 1 while createH264DecoderFor may open the TEE Decoder: policy auto and not switched off for this run.
int isH264TeeUsable(void);

// after the TEE Decoder failed while playing: cellVdec only until the app restarts. `why` goes to the log.
void disableH264TeeForSession(const char *why);

typedef struct {
   int width, height;            // coded size (see above)
   int level;                    // AVCLevelIndication
   int maxRefFrames;             // SPS max_num_ref_frames
   const uint8_t *header;        // the stream's SPS + PPS as Annex B (the demuxer's H264Config.header)
   int headerSize;
   int spsPatched;               // H264Config.spsPatched: the header's reference count was rewritten for cellVdec
} H264StreamParams;

// the TEE Decoder when the policy allows it and it can decode this stream (High or Main profile, CABAC, 8 bit
// 4:2:0, progressive, no scaling matrices, a coded width that is a multiple of 128 up to 1920 x 1088, enough
// memory), cellVdec otherwise. NULL when neither opens.
H264Decoder *createH264DecoderFor(const H264StreamParams *params);

const char *getH264DecoderName(const H264Decoder *decoder);   // "TEE Decoder" or "cellVdec"
int  isTeeH264Decoder(const H264Decoder *decoder);

// feeds one Annex-B access unit. pts is opaque (passed through to the decoded picture). returns 0 when fed (or,
// while catch-up skip is on, dropped as a non-reference picture), 1 when the decoder's queues are full (pull
// pictures with getFrameH264, then retry the same AU), -1 on error. cellVdec reads `data` in place, so it must
// stay untouched until the backlog says the decoder has consumed it; the TEE Decoder copies it at once.
int decodeAuH264(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts);

// access units fed but not yet consumed (cellVdec) or not yet out as pictures (TEE Decoder).
int getAuBacklogH264(const H264Decoder *decoder);

// how far the caller may let that backlog grow before it stops demuxing. cellVdec reads the AU buffers in place,
// so it is the demuxer's AU_BUFFER_COUNT; the TEE Decoder copies every AU and wants its CABAC stage well ahead of
// the pixel stage, so it is much larger - and a backlog that sits at the limit with no picture coming out is
// how the caller's stall detector notices a stream the decoder accepts but never decodes.
int getAuBacklogLimitH264(const H264Decoder *decoder, int auBufferCount);

// while enabled, non-reference pictures are dropped: cellVdec decodes with CELL_VDEC_DEC_MODE_B_SKIP, the TEE
// Decoder is not fed access units whose slices have nal_ref_idc 0 - the one way to hold real time on a stream
// the SPUs cannot decode in full.
//
// Deliberately not left on: YouTube's VOD encodes are ~87% disposable B-frames, so skipping them all
// the time leaves a slideshow. The decoder cannot tell whether it is behind (its AU backlog is a
// pipeline depth the decode thread keeps full on purpose), so the player decides: video-player.c turns
// this on when the decoded picture is more than 250 ms behind the clock it is paced to and off again
// under 60 ms, and only for streams above 30 fps.
// (TEE Vanced addition, 2026-09-01; the first version gated on the backlog and dropped 720p60 to 35-40 fps.)
void setH264CatchUpSkip(H264Decoder *decoder, int enable);

// no more access units: the TEE Decoder hands out the pictures it still holds (its reorder queue), then reports
// drained. cellVdec needs nothing (the caller drains it for a while).
void endOfStreamH264(H264Decoder *decoder);
int  isH264Drained(const H264Decoder *decoder);   // TEE Decoder: every picture out after endOfStreamH264

// retrieves a decoded frame as YUV 4:2:0 planar into `yuvOut` (must hold codedWidth*codedHeight*3/2
// bytes, 128-byte aligned, size a multiple of 128). frames come out in presentation order. returns 1 and
// sets *outWidth/*outHeight/*outPts (the pts passed in for that frame's AU) when a frame was ready,
// 0 if none pending, -1 on error. cellVdec reports the coded size, the TEE Decoder the visible one (1080 rows,
// not 1088), packed at that size.
int getFrameH264(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts);

// diagnostic: the system time (microseconds) at which the decoder reported the LAST frame retrieved by
// getFrameH264 as ready (its PICOUT). Compare against when the AU was fed to get the decoder's own
// latency, and against now to get how long the ready picture waited for the caller to pull it. Only
// meaningful straight after getFrameH264 returned 1 (cellVdec; 0 for the TEE Decoder).
uint64_t getDecoderReadyUs(const H264Decoder *decoder);

// flushes the decode pipeline for a seek: ends the sequence (discarding in-flight pictures) and
// starts a fresh one on the same handle. Feed the new position's keyframe next. 0 ok, -1 on error.
int resetH264Decoder(H264Decoder *decoder);

void destroyH264Decoder(H264Decoder *decoder);

#pragma once

// video-player - the playback engine. Owns the demuxer (MKV or MP4), the H.264 decoder and a
// background thread that decodes ahead into a small ring of frames. The UI presents frames by
// calling getVideoFrame each render frame; the engine hands back the frame whose presentation time
// is due (skipping ahead if the UI fell behind, holding if it's early), so playback runs at the
// file's real frame rate. The AAC audio track drives the clock while it plays; wall-time otherwise.

#include <stdint.h>

typedef struct VideoPlayer VideoPlayer;

// frame-buffer allocator hooks. The UI supplies these so frames land in RSX-visible memory and are
// drawn zero-copy (gfx's allocGfxVideoBuffer/freeGfxVideoBuffer); pass NULL for plain heap buffers.
typedef void *(*VideoFrameAllocFn)(size_t size);
typedef void  (*VideoFrameFreeFn)(void *buffer);

// opens `path`, brings up the decoder, and starts decoding. Returns NULL if the file can't be
// demuxed / decoded (caller should have probed first for a user-facing reason).
VideoPlayer *createVideoPlayer(const char *path, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame);

// like createVideoPlayer, but the audio comes from a SEPARATE source (adaptive/DASH: a video-only
// url plus an audio-only url). audioPath NULL means the audio is muxed into videoPath. A/V stay in
// sync because both share one presentation timeline and the audio drives the clock.
VideoPlayer *createVideoPlayerSplit(const char *videoPath, const char *audioPath, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame);
void         destroyVideoPlayer(VideoPlayer *player);

// returns the YUV 4:2:0 planar frame (Y then U then V, width*height*3/2 bytes) that should be shown
// now (paced against the clock), or NULL if none is ready yet. The pointer stays valid until the
// SECOND-next getVideoFrame call (the previous frame is retired one call late so the GPU is never
// reading a buffer the decoder is refilling). Fills *width/*height with the coded plane dimensions.
const uint8_t *getVideoFrame(VideoPlayer *player, int *width, int *height);

void  setVideoPaused(VideoPlayer *player, int paused);
int   isVideoPaused(const VideoPlayer *player);
// asynchronous seek: decode restarts from the nearest cued keyframe at or before `seconds` (both
// pipelines flush; the current frame stays on screen until the new position's frames arrive).
// Seeking after the end restarts playback.
void  seekVideoPlayer(VideoPlayer *player, float seconds);
// like seekVideoPlayer but lands on the next keyframe at or AFTER `seconds`, so a skip clears the whole
// span (e.g. a sponsor segment) instead of resuming on the keyframe before it and replaying the tail.
void  seekVideoPlayerPast(VideoPlayer *player, float seconds);
int   isVideoEnded(const VideoPlayer *player);       // decode reached end of stream and the ring drained
float getVideoPositionSeconds(const VideoPlayer *player);
// the same in nanoseconds, exact: a live stream's pts count from its broadcast's start, and after months on air
// (sq 4174810, 241 days, seen 2026-11-07) a float second has whole seconds of step
uint64_t getVideoPositionNs(const VideoPlayer *player);
float getVideoDurationSeconds(const VideoPlayer *player);
// a live segment stream: the sq segment its picture began with (see live-source.h); -1 for anything else
long  getVideoLiveFirstSq(const VideoPlayer *player);
void  getAudioTrackInfo(const VideoPlayer *player, int *rate, int *channels);   // 0/0 if no audio track

// the decoder behind the picture, for a stats overlay: "TEE Decoder" or "cellVdec" (see decode-h264.h)
const char *getVideoDecoderName(const VideoPlayer *player);

// 1 when the picture comes from the TEE Decoder or the TEE VP9 building block (0: cellVdec)
int isVideoOnTeeDecoder(const VideoPlayer *player);

// (TEE Vanced V3.0.0) VP9 - YouTube's 1440p, in WebM - plays on the TEE VP9 building block (src/tee_vp9/, vendored
// unchanged). 1440p60 needs all 6 SPEs (2 entropy + 4 pixel), so Sony's sound decoder (cellAdec, 1 SPE) has no
// room: spes 6 (the default) plays the picture without sound; 5 gives the decoder 1 + 4 SPEs and keeps the sound
// (slower: 50 - 63 pictures a second measured, so 1440p60 cannot hold 60). Process-wide; settings.txt vp9-spes=.
void setVideoVp9Spes(int spes);
int  isVideoVp9(const VideoPlayer *player);      // the picture comes from the TEE VP9 building block
int  isAudioOpus(const VideoPlayer *player);     // (V3.0.0) the sound is Opus, on TEE Opus (VP9's own track)

// where the planes of the picture getVideoFrame last handed out lie, for drawing it in place. A packed frame (H.264:
// Y, U, V back to back) fills it as one block without a border; a VP9 picture is the decoder's own buffer: three
// planes with rows `pitch` apart and a border around the picture. 0 when nothing is on screen yet.
typedef struct {
   const uint8_t *y, *u, *v;         // each plane's first byte (border included), 128-byte aligned
   int pitchY, pitchC;               // bytes from one row to the next
   int planeW, planeH;               // the luma plane in samples (border included)
   int planeWC, planeHC;             // the chroma planes
   int left, top, width, height;     // the picture inside the luma plane
   int leftC, topC;                  // its top left inside the chroma planes
} VideoFrameLayout;
int getVideoFrameLayout(VideoPlayer *player, VideoFrameLayout *layout);

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
// (V3.0.0 Test 14) as createVideoPlayerSplit, for VP9 1440p with its 1080p VP9 stream beside it (YouTube's 303 for 308,
// 248 for 271; lowVideoPath NULL = none): the player changes to the 1080p stream ahead of a stretch the decoder cannot
// keep at 60 in 1440p - by the bitrate the 1440p stream's cues give for the next 10 s - and back after it. One decoder,
// one sound: the sound runs on through a change. A 1080p stream that does not fit (another rate, no cues) is left out.
VideoPlayer *createVideoPlayerVp9(const char *videoPath, const char *lowVideoPath, const char *audioPath,
                                  VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame);
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
// (V3.0.4) the decoder stopped with an error it does not come back from (TEE VP9 on a frame it cannot take, where the
// player cannot go on in 1080p VP9 by itself): the picture stands, isVideoEnded says 1 - the app had better build the
// player anew, in a quality that plays. A seek starts it over.
int   hasVideoPlayerFailed(const VideoPlayer *player);
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
int  isVideoOnLowTrack(const VideoPlayer *player);   // (Test 14) 1080p VP9 in place of 1440p: the picture on screen
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

// ---- (V3.1.0 Test 3) the display's counters, and the switches the bench (apps/yo-player bench.c) turns ----
// The app's gfx layer counts flips and vertical blanks; main.c hands them over after initGfx. Without them the player
// paces by wall time and gives a picture back one present after it left the screen, as before.
typedef struct {
   uint32_t (*nextFlipId)(void);   // the number the flip of the frame being drawn will have (flips queued + 1)
   uint32_t (*flipsDone)(void);    // flips the screen has shown so far
   uint32_t (*vblanksMissed)(void);   // vertical blanks the screen showed a frame again for (by the flips' times)
   // (V3.1.0 Test 4) TEE VP9's RSX part (gfx.h): may it run now; can the RSX read this memory; around the close of a decoder
   // with it, the screen paused and the RSX drained (0 drained), and going on; the RSX part off for the run; the crash marker
   // on or off. NULL (or the first one 0): never the RSX part
   int  (*rsxAllowed)(void);
   int  (*isMapped)(const void *address);
   int  (*pause)(uint32_t timeoutUs);
   void (*resume)(void);
   void (*brake)(const char *why);
   void (*rsxActive)(int on);
} VideoDisplayHooks;
void setVideoDisplayHooks(const VideoDisplayHooks *hooks);
// (V3.1.0 Test 4) settings.txt vp9-rsx=off|1440|on (0 / 1 / 2): TEE VP9 opened with its RSX part (rsx = 1) - up to 1440p, or
// 4K too - when the display shares the RSX (gfx-share=on, hooks.rsxAllowed). Its pictures are drawn only once it predicts
// and has mapped them (else after 30 pictures the player fails with didVideoPlayerFailOnRsx, and the RSX part is off for the
// run: the app builds the player anew where it was). test: settings.txt rsx-test= ("brake-after-30", "stall-3000"), "" none
void setVideoVp9Rsx(int mode);
void setVideoVp9RsxTest(const char *test);
int  didVideoPlayerFailOnRsx(const VideoPlayer *player);
// settings.txt display-sync=flips|wall: display sync advances the picture by one per present plus the vertical blanks the
// screen showed the last frame again for (1, the default), or by the wall time between two presents, rounded (0: V3.1.0
// Test 2 - a present that ran late but still made its vblank counted 2 and the next one 0: a dropped and a repeated
// picture while the screen missed nothing)
void setVideoDisplaySync(int byFlips);
// vp9-release=early|late (the bench's A/B): a picture goes back to TEE VP9 as soon as the flip after it has happened
// and while the decode thread waits for the next one (1, the default), or at the next present with a new picture (0)
void setVideoVp9ReleaseEarly(int early);

// what a player has done since it was built - the bench reads it at the start and the end of a run (UI thread)
typedef struct {
   uint32_t presents;       // getVideoFrame calls after the pre-roll
   uint32_t shown;          // ... with a new picture
   uint32_t late;           // pictures dropped as late
   uint32_t repeats;        // presents without a new picture
   uint32_t starved;        // ... with none ready at all
   uint32_t holds, holdMs;  // the sound waited for the picture: how often, how long in all
   uint32_t toLow, toHigh;  // VP9: changes to the lower stream (1080p / 1440p) and back
   uint32_t safetyNets;     // ... the safety net's words
   uint32_t restarts;       // ... TEE VP9 opened anew after a stop
   int      memFreeMb;      // user memory free once the decoder was open (-1: not known)
   int      width, height;  // the main stream
   int      fpsMilli;       // its frame rate in mHz (0: not known yet)
   int      onLow;          // the picture on screen comes from the lower stream
   // TEE VP9's own numbers (decode thread, copied about once a second); all 0 for H.264
   int      vp9Frames, vp9Pictures, vp9Skipped, vp9RsxFrames, vp9EntropyWaits, vp9RecordsWaits;
   int      vp9BusyFpsX10;  // its "pictures a second while busy", times 10
} VideoPlayerCounters;
void getVideoPlayerCounters(VideoPlayer *player, VideoPlayerCounters *counters);

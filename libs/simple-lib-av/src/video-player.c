// video-player - streaming playback engine. See video-player.h.
//
// A background thread demuxes + decodes ahead into a ring of frame slots. The UI thread pulls the
// due frame each render via getVideoFrame. A slot moves EMPTY -> FILLING (claimed by the decode
// thread) -> READY (decoded, awaiting its presentation time) -> DISPLAYING (handed to the UI) ->
// RETIRING (replaced, held one extra present while the RSX may still sample it) -> EMPTY. The decode
// thread only ever claims EMPTY slots, so a frame the GPU can see is never overwritten under it.
#include "video-player.h"
#include "demux.h"
#include "h264.h"
#include "decode-h264.h"
#include "decode-aac.h"
#include "tee_vp9/tee_vp9.h"   // TEE's VP9 decoder on the SPEs (vendored unchanged, see tee_vp9.sha256)
#include "../tee_opus/tee_opus.h"   // TEE's Opus decoder on the PPU: VP9's sound (vendored unchanged)
#include "audio.h"
#include "thread.h"
#include "dbg.h"
#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>
#include <sys/memory.h>   // sys_memory_get_user_memory_size: the VP9 decoder's memory, in the log
#include <sysutil/sysutil_sysparam.h>   // cellVideoOutGetState: the screen's refresh rate (display sync)

// the decode thread runs ahead into this ring; the extra slots past PREROLL_FRAMES are cushion the
// decoder banks during easy scenes and spends on hard (high-bitrate) frames without the picture stalling.
#define FRAME_SLOTS           8
#define PREROLL_FRAMES        4      // frames buffered before the first present (start latency, not ring depth)
#define PREROLL_AUDIO_TIMEOUT_US (5 * 1000 * 1000)   // give up waiting for audio to anchor the clock, play video alone
#define DECODE_STALL_MS       3000   // no AU consumed and no picture produced for this long = hard error
#define END_OF_STREAM_DRAIN_MS 500   // how long to wait for the last pictures after the final AU

// RETIRING holds a replaced frame for one extra present: the RSX may still be sampling it for the
// frame in flight, so the decoder must not refill it until the following present.
// RELEASE (VP9 only): done with, but its picture still belongs to the TEE VP9 decoder - the decode thread gives
// it back (tvp9_release_picture: the module wants get and release from one thread) and empties the slot.
enum { SLOT_EMPTY, SLOT_FILLING, SLOT_READY, SLOT_DISPLAYING, SLOT_RETIRING, SLOT_RELEASE };

typedef struct {
   uint8_t *yuv;        // YUV 4:2:0 planar, RSX-visible when the UI supplied the allocator (NULL for VP9)
   uint64_t pts;        // nanoseconds
   int      width, height;
   int      state;
   tvp9_picture vp9;    // VP9: the decoder's picture this slot shows, drawn from the decoder's own memory
   int      hasVp9;     // vp9 holds a picture that goes back to the decoder when the slot is done with
} FrameSlot;

// VP9 (TEE VP9 building block): the slots hold the decoder's pictures, not copies of them. A held picture keeps
// one of its buffers, so the ring stays short: 2 on screen (DISPLAYING + RETIRING) and 2 ready - the decoder's
// own queue of finished pictures is the cushion beyond. A 1440p picture is 5.5 MB: copying 60 a second on the
// PPU (330 MB/s) is what the module's guide warns against.
#define VP9_SLOTS          4
#define VP9_PREROLL_FRAMES 2
#define VP9_RATE_PICTURES  30     // a WebM without a frame duration: the rate is measured over this many pictures
#define VP9_STALL_MS       5000   // no picture and nothing fed for this long = the stream is dead
#define VP9_RECOVERIES     5      // (Test 12) read failures a player tries again from where they stopped

// (Test 2) what the decoder is opened with, the first that fits: picture buffers (its default 10 + 2 for the ring
// above, 6.5 MB each at 1440p) and how much input it may hold before it says "full". YouTube's 1440p60 averages
// about 10 Mbit/s but runs at 27 - 37 Mbit/s for half a minute in a busy scene (Elden Ring DLC at 17:11, Test 1):
// more than the PS3 downloads. The input is the cushion that rides such a stretch out: 20 MB is its queue of
// 256 frames (about 4 s) at the peak rate, the module's default 6 MB a little over 1 s of it.
static const struct { int pictures; uint32_t inputMb; } VP9_SETUPS[] = { { 12, 20 }, { 10, 20 }, { 10, 6 } };

static int vp9Spes = 6;           // see setVideoVp9Spes

struct VideoPlayer {
   VideoDemuxer demuxer;
   H264Decoder *decoder;
   H264StreamParams streamParams; // what the decoder was built for: rebuilt from it after a failure
   int          inputEnded;       // the demuxer hit the end of the stream and the decoder was told
   uint64_t     decoderFailureUs; // when the TEE Decoder last failed while playing (0 = never)
   int          allocW, allocH;   // coded frame-buffer dimensions (macroblock-aligned)
   int          catchUpAllowed;   // >30fps stream: decoder MAY drop B pictures while it is behind
   VideoFrameFreeFn freeFrame;    // releases the slot buffers (matches the allocator used)

   // VP9 (the demuxer found a V_VP9 track): the TEE VP9 building block instead of the H.264 decoder
   tvp9        *vp9;
   void        *vp9Memory;        // its picture buffers, from the UI's allocator (mapped for the RSX, 1 MB aligned)
   // (Test 2) the VP9 input has a thread of its own: a read that waits on the network (40 - 330 ms at a time in
   // Test 1) then holds up the feeding only, while the decode thread keeps handing finished pictures to the screen
   sys_ppu_thread_t vp9FeedTid;
   int              vp9FeedActive;
   volatile int     vp9FeedExited;
   volatile int     vp9FeedPause;     // decode thread -> feeder: park (a seek)
   volatile int     vp9FeedParked;    // feeder -> decode thread: parked, the demuxer and the decoder's input are free
   volatile int     vp9FeedError;     // the stream could not be read
   volatile uint32_t vp9Fed;          // blocks fed so far (the decode thread's stall check)
   int              vp9StartedAtKeyframe;   // feeder-owned: the first key frame since the open / the last seek
   int              vp9Recoveries;          // (V3.0.0 Test 12) read failures tried again from where they stopped
   uint64_t     vp9StatsUs;       // when the decoder's numbers last went to the log
   uint64_t     vp9FirstPts;      // the rate measurement for a WebM without a frame duration
   int          vp9RatePictures;

   VideoAu      pendingAu;        // demuxed AU not yet accepted by the decoder (queues were full)
   int          hasPendingAu;

   FrameSlot     slots[FRAME_SLOTS];
   sys_lwmutex_t lock;

   sys_ppu_thread_t thread;
   int              threadActive;
   volatile int     running;      // decode thread runs while set
   volatile int     decodeExited; // the decode thread has left (destroyVideoPlayer waits for this, bounded)
   volatile int     ended;        // demux hit end of stream

   // audio pipeline (absent for files without an AAC track: video plays silent)
   AacDecoder      *aacDecoder;
   topus           *opus;               // (V3.0.0) an Opus audio stream (YouTube's beside VP9): decoded on the PPU
   VideoDemuxer     audioDemuxer;       // separate audio-only source for split (adaptive) streams
   int              hasAudioDemuxer;    // audioDemuxer is open and supplies the audio (else it's in `demuxer`)
   sys_ppu_thread_t audioThread;
   int              audioThreadActive;
   int              audioFeedOpen;      // mixer external-PCM feed is ours to close
   uint8_t         *adtsBuffers[2];     // rotating ADTS frame scratch (7-byte header + raw AAC frame)
   float           *pcmScratch;         // one decoded frame as stereo, en route to the mixer feed

   // audio-master clock: video paces against samples the mixer actually consumed, so A/V can't drift
   volatile int     audioAlive;         // audio thread is running (cleared on exit/error)
   volatile int     audioClockValid;    // audioClockBasePts anchored by the first decoded frame
   uint64_t         audioClockBasePts;  // pts (ns) of the first PCM frame handed to the mixer feed

   // the audio clock smoothed for the picture (see presentationClockNs): how far it is from wall time, filtered
   // by the UI thread at every present, read by the decode thread for its catch-up check
   volatile int     clockSmoothValid;
   volatile int64_t clockOffsetNs;

   // seek: the UI posts a target; the decode thread performs it (parking the audio thread while both
   // pipelines flush), then decode restarts from the nearest earlier cued keyframe
   volatile int64_t seekRequestNs;      // pending target in ns, -1 = none
   int              seekLandAfter;      // 1 = land on the next keyframe at/after the target (sponsor skip), not before
   volatile int     audioFlushRequest;  // decode thread -> audio thread: flush and park
   volatile int     audioFlushDone;     // audio thread -> decode thread: flushed, parked
   volatile int64_t audioSeekPendingNs; // decode thread -> audio thread: seek your demuxer here (-1 = none); keeps
                                        // the audio stream's network round trips off the picture's critical path

   // presentation clock (wall-time based until audio drives it)
   int      started;              // 1 once the first frame's pts anchored the clock
   uint64_t basePts;              // pts of the first presented frame (ns)
   uint64_t baseWallUs;           // wall time the clock was anchored (us)
   int      paused;
   uint64_t pauseWallUs;
   uint64_t currentPts;           // pts of the frame currently displayed
   int      displayIndex;         // slot being displayed, or -1
   uint64_t audioWaitStartUs;     // when pre-roll first started waiting on the audio clock (0 = not yet)

   // presentation statistics (getVideoFrame, under lock): a line goes to the log for every second in which a
   // picture was dropped as late or none was ready - the stutter a viewer sees, with what was behind it
   uint64_t statsSecondUs;
   int      statsShown, statsLate, statsRepeats, statsStarved, statsReadyMin;

   // display sync (see pickSyncedFrame): a video whose frame rate matches the screen's (60 fps on 59.94 Hz, 30 on
   // 59.94 at two vblanks a picture) shows exactly one picture per vblank, and the sound's tempo is steered by a
   // hair so the clock keeps up - instead of the clock dropping a picture every 1 / (60 - 59.94) = 16.7 s
   int      syncPerFrame;       // vblanks per picture when the rates match (1 or 2), 0 = no display sync
   double   syncBaseSpeed;      // the sound's tempo that matches: screen rate / (syncPerFrame x frame rate)
   uint64_t syncVblankUs;       // one vblank of the screen
   int      syncActive;         // frame-locked right now (UI thread, under lock)
   uint64_t syncLastUs;         // when the last present ran
   int      syncVblanks;        // vblanks counted toward the next picture
   int      syncSpeedSet;       // the sound's tempo is not 1.0 at the moment
   // (V3.0.0) VP9: the sound waits for the picture. Its Opus track is a stream of its own, a few KB a second, so a
   // picture short of input (or of decoding time) fell behind a sound that ran on - up to 0.7 s in the hardest
   // minutes of a 1440p60 stream (TEE, 2026-11-12). Past AV_HOLD_ENGAGE_NS behind, the sound pauses (clock frozen)
   // until the picture has caught up, the way a player waits while it buffers.
   int      audioHeld;
   uint64_t audioHeldSinceUs;
   uint64_t syncLogUs;          // when the last display-sync line went to the log
};

// defined below the decode thread but used by performSeek's audio-revive path
static VideoDemuxer *getAudioSource(VideoPlayer *player);
static void audioDecodeThread(uint64_t arg);
static void opusAudioThread(uint64_t arg);   // (V3.0.0) the Opus one, beside it below
static void setUpDisplaySync(VideoPlayer *player);   // a VP9 stream whose rate the decode thread measured
static void postSeek(VideoPlayer *player, float seconds, int landAfter);   // (Test 12) the VP9 feeder's retry

// how far the freshly decoded picture may fall behind the clock before the decoder is allowed to
// drop non-reference B pictures, and where it stops again. Hysteresis so it cannot flap per frame.
#define CATCHUP_ENGAGE_NS  250000000ull   // 250 ms behind: give up on decoding every picture
#define CATCHUP_RELEASE_NS  60000000ull   // back within 60 ms: decode in full again

// where the audio is: the samples the mixer has consumed since the clock was anchored
static uint64_t audioClockNs(VideoPlayer *player)
{
   return player->audioClockBasePts +
          getAudioPcmFeedPlayedFrames() * 1000000000ull / (uint64_t)getAudioSource(player)->audioRate;
}

#define CLOCK_SNAP_NS   40000000ll   // further than this from the audio (a seek, a stall): jump, don't glide
#define AV_HOLD_ENGAGE_NS  100000000ll   // (V3.0.0, VP9) the picture this far behind the sound: the sound waits
#define AV_HOLD_READY      2             // ... until the picture has caught up and both ready slots are full (VP9_SLOTS)
#define AV_HOLD_MAX_US     2000000ull    // ... or, caught up, after this long whatever is decoded
#define CLOCK_FILTER    32           // the smoothed clock moves 1/32 of the way to the audio at every present

// the clock the picture is paced against: the audio feed while it runs, wall time otherwise.
// Shared by the presentation loop and the decode thread's catch-up check so both judge by the
// same measure.
//
// The audio clock only moves once per mixer block (256 samples at 48 kHz, 5.3 ms), and with the mixer
// thread's wake-up jitter on top: a staircase. Paced straight against it, a 60 fps picture due within a few ms
// of a vblank flipped between that vblank and the next from one present to the other - and a 60 fps video on a
// 59.94 Hz screen drifts through exactly that alignment every 1 / (60 - 59.94) = 16.7 s. For about 6 s each time
// the presenter dropped 8 pictures a second as late and repeated as many: the judder TEE saw at 1080p60 on the
// TEE Decoder (Test3, 2026-11-07: fewest ready frames 6 of 8 throughout, so never the decoder). So the picture
// runs on wall time plus a filtered estimate of the audio's offset from it: that follows the audio's drift, not
// its steps, and the 16.7 s beat costs one dropped picture instead of six seconds of judder.
static uint64_t presentationClockNs(VideoPlayer *player, uint64_t nowUs)
{
   if (player->audioClockValid && player->started) {
      if (player->paused) return player->currentPts;
      if (player->clockSmoothValid) return (uint64_t)((int64_t)(nowUs * 1000) + player->clockOffsetNs);
      return audioClockNs(player);
   }
   if (player->paused) return player->currentPts;
   // VP9 without sound (all 6 SPEs on the picture): with display sync the wall clock runs at the screen's pace -
   // 0.999 for 60 fps on 59.94 Hz - so one picture per vblank never falls behind it (the sound does this with its
   // tempo when there is sound)
   if (player->vp9 && player->syncPerFrame && !player->audioFeedOpen)
      return player->basePts + (uint64_t)((double)(nowUs - player->baseWallUs) * 1000.0 * player->syncBaseSpeed);
   return player->basePts + (nowUs - player->baseWallUs) * 1000;
}

// UI thread, once per present (under player->lock): steers the smoothed clock toward the audio clock
static void steerPresentationClock(VideoPlayer *player, uint64_t nowUs)
{
   if (!player->audioClockValid || !player->started || player->paused || player->audioHeld) { player->clockSmoothValid = 0; return; }
   int64_t offset = (int64_t)audioClockNs(player) - (int64_t)(nowUs * 1000);
   int64_t error  = offset - player->clockOffsetNs;
   if (!player->clockSmoothValid || error > CLOCK_SNAP_NS || error < -CLOCK_SNAP_NS) {
      player->clockOffsetNs = offset;
      __sync_synchronize();
      player->clockSmoothValid = 1;
   } else {
      player->clockOffsetNs += error / CLOCK_FILTER;
   }
}

// ============================================================================
// decode thread
// ============================================================================

// VP9 (caller holds player->lock, decode thread): gives the pictures of the slots the UI is done with back to
// the decoder and empties those slots
static void releaseVp9Slots(VideoPlayer *player)
{
   for (int i = 0; i < FRAME_SLOTS; i++) {
      FrameSlot *slot = &player->slots[i];
      if (slot->state != SLOT_RELEASE) continue;
      if (slot->hasVp9) tvp9_release_picture(player->vp9, &slot->vp9);
      slot->hasVp9 = 0;
      slot->state = SLOT_EMPTY;
   }
}

// the UI is done with a slot (caller holds player->lock): empty - or, with a VP9 picture in it, RELEASE, so the
// decode thread gives the picture back first
static void retireSlot(VideoPlayer *player, int i)
{
   player->slots[i].state = player->slots[i].hasVp9 ? SLOT_RELEASE : SLOT_EMPTY;
}

// what a slot shows: its YUV buffer, or a VP9 picture's luma (non-NULL is all getVideoFrame's callers ask)
static const uint8_t *slotPicture(const FrameSlot *slot)
{
   return slot->hasVp9 ? slot->vp9.y : slot->yuv;
}

// claims a free slot for the decode thread to fill; returns its index or -1 if the ring is full.
static int claimEmptySlot(VideoPlayer *player)
{
   int found = -1;
   lock(&player->lock);
   if (player->vp9) {
      releaseVp9Slots(player);
      int inUse = 0;
      for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].state != SLOT_EMPTY) inUse++;
      if (inUse >= VP9_SLOTS) { unlock(&player->lock); return -1; }
   }
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_EMPTY) { player->slots[i].state = SLOT_FILLING; found = i; break; }
   unlock(&player->lock);
   return found;
}

// VP9: feeds the demuxer's blocks while the decoder takes them (it copies each one at once and says "full" -
// 0 - when it has enough). Returns how many it fed, -1 on a read error. At the end of the input it tells the
// decoder and sets inputEnded.
static int feedVp9(VideoPlayer *player, int *startedAtKeyframe)
{
   int fed = 0;
   while (fed < 8 && player->running && player->seekRequestNs < 0 && !player->inputEnded) {
      if (!player->hasPendingAu) {
         uint64_t readStartUs = sys_time_get_system_time();
         int r = readVideoAu(&player->demuxer, &player->pendingAu);
         uint64_t readUs = sys_time_get_system_time() - readStartUs;
         if (readUs > 30000) logWarn("[video-player] reading the next VP9 block took %d ms\n", (int)(readUs / 1000));
         if (r < 0) return -1;
         if (r == 0) { player->inputEnded = 1; tvp9_end_of_stream(player->vp9); break; }
         if (!*startedAtKeyframe) { if (!player->pendingAu.keyframe) continue; *startedAtKeyframe = 1; }
         player->hasPendingAu = 1;
      }
      int result = tvp9_feed(player->vp9, player->pendingAu.data, (size_t)player->pendingAu.size, (int64_t)player->pendingAu.pts);
      if (result == 0) break;            // the decoder has enough: retry this block later
      if (result < 0) logWarn("[video-player] the VP9 decoder refused a block of %d bytes\n", player->pendingAu.size);
      player->hasPendingAu = 0;
      player->vp9Fed++;
      fed++;
   }
   return fed;
}

// VP9 feeder thread (Test 2): reads the demuxer and feeds the decoder, nothing else. Parks while the decode thread
// seeks (vp9FeedPause: the demuxer moves and the decoder flushes - tvp9_flush wants nothing fed meanwhile).
static void vp9FeedThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;
   while (player->running) {
      if (player->vp9FeedPause) { player->vp9FeedParked = 1; __sync_synchronize(); sleepMs(1); continue; }
      player->vp9FeedParked = 0;
      if (player->inputEnded || player->vp9FeedError || player->seekRequestNs >= 0) { sleepMs(2); continue; }
      int fed = feedVp9(player, &player->vp9StartedAtKeyframe);
      if (fed < 0) {
         if (player->running && player->seekRequestNs < 0) {
            // (V3.0.0 Test 12) the network failed: try again from the picture on screen (a seek reconnects the stream)
            // a few times before giving up - a frozen picture with its sound running on (Test 11) helps nobody
            if (player->vp9Recoveries < VP9_RECOVERIES) {
               player->vp9Recoveries++;
               logWarn("[video-player] reading the VP9 stream failed: trying again from %d s (%d of %d)\n",
                       (int)(player->currentPts / 1000000000ull), player->vp9Recoveries, VP9_RECOVERIES);
               postSeek(player, (float)((double)player->currentPts / 1.0e9), 0);
            } else {
               logError("[video-player] reading the VP9 stream failed\n");
               player->vp9FeedError = 1;
            }
         }
         continue;
      }
      if (fed == 0) sleepMs(2);          // the decoder is full: it is ahead
   }
   __sync_synchronize();
   player->vp9FeedExited = 1;
   exitThread();
}

// VP9: the next picture into *pic (decode thread; the feeder fills the decoder meanwhile). 1 with a picture, 0 at the
// end of the stream, -1 on an error. Waiting on a slow network is no error as long as blocks keep coming in.
static int decodeNextVp9(VideoPlayer *player, tvp9_picture *pic)
{
   int stalledMs = 0;
   uint32_t fedSeen = player->vp9Fed;
   while (stalledMs < VP9_STALL_MS && player->running && player->seekRequestNs < 0) {
      int got = tvp9_get_picture(player->vp9, pic);
      if (got == 1) return 1;
      if (got < 0) {
         const char *why = tvp9_error(player->vp9);
         if (!why) return 0;              // every picture after the end of the stream is out
         logError("[video-player] TEE VP9 stopped: %s\n", why);
         return -1;
      }
      if (player->vp9FeedError) return -1;
      if (player->vp9Fed != fedSeen) { fedSeen = player->vp9Fed; stalledMs = 0; }
      sleepMs(1); stalledMs++;
   }
   if (player->running && player->seekRequestNs < 0 && stalledMs >= VP9_STALL_MS) {
      if (player->inputEnded) return 0;
      logError("[video-player] TEE VP9: no picture and no input for %d ms\n", VP9_STALL_MS);
      return -1;
   }
   return 0;
}

// VP9, after each picture (decode thread): the decoder's numbers to the log every 10 s, and - for a WebM whose
// track has no frame duration - the frame rate from the first pictures' time stamps, so display sync can lock on
static void afterVp9Picture(VideoPlayer *player, const tvp9_picture *pic)
{
   uint64_t nowUs = sys_time_get_system_time();
   if (!player->vp9StatsUs) player->vp9StatsUs = nowUs;
   else if (nowUs - player->vp9StatsUs >= 10000000ull) {
      player->vp9StatsUs = nowUs;
      tvp9_stats stats;
      tvp9_get_stats(player->vp9, &stats);
      // integers only: this printf has no floating point
      logInfo("[video-player] TEE VP9: %d frames, %d pictures, %d.%d pictures/s while busy, entropy %d us, pixel %d us a frame\n",
              stats.frames, stats.pictures, (int)stats.fps, (int)(stats.fps * 10.0) % 10,
              (int)(stats.entropy_ms * 1000.0), (int)(stats.pixel_ms * 1000.0));
   }
   if (player->demuxer.frameDurationNs) return;
   if (player->vp9RatePictures == 0) player->vp9FirstPts = (uint64_t)pic->pts;
   if (++player->vp9RatePictures <= VP9_RATE_PICTURES) return;
   uint64_t span = (uint64_t)pic->pts > player->vp9FirstPts ? (uint64_t)pic->pts - player->vp9FirstPts : 0;
   player->vp9RatePictures = 0;
   if (!span) return;
   lock(&player->lock);
   player->demuxer.frameDurationNs = span / VP9_RATE_PICTURES;
   setUpDisplaySync(player);
   unlock(&player->lock);
}

// feeds demuxed access units while the decoder takes them and its backlog is below the limit. Returns how many
// it fed (0: the decoder is full, at its limit, or the input ended), -1 on an error. At the end of the input it
// tells the decoder and sets inputEnded.
//
// The demuxer rotates AU_BUFFER_COUNT buffers, which cellVdec reads in place: it gets one AU per call, the
// backlog kept as full as those buffers allow (the SDK notes throughput improves when the queue never runs dry)
// without overwriting an AU it is still reading. The TEE Decoder copies every AU at once and wants its CABAC
// stage far ahead of its pixel stage, so it gets several per call, up to a much deeper backlog.
static int feedDecoder(VideoPlayer *player, int *startedAtKeyframe)
{
   int tee = isTeeH264Decoder(player->decoder);
   int limit = getAuBacklogLimitH264(player->decoder, AU_BUFFER_COUNT);
   int fed = 0;
   while (fed < (tee ? 8 : 1) && player->running && player->seekRequestNs < 0 && !player->inputEnded) {
      if (!player->hasPendingAu && getAuBacklogH264(player->decoder) >= limit) break;
      if (!player->hasPendingAu) {
         uint64_t readStartUs = sys_time_get_system_time();
         int r = readVideoAu(&player->demuxer, &player->pendingAu);
         uint64_t readUs = sys_time_get_system_time() - readStartUs;
         if (readUs > 30000) logWarn("[video-player] reading the next access unit took %d ms\n", (int)(readUs / 1000));
         if (r < 0) return -1;
         if (r == 0) { player->inputEnded = 1; endOfStreamH264(player->decoder); break; }
         if (!*startedAtKeyframe) { if (!player->pendingAu.keyframe) continue; *startedAtKeyframe = 1; }
         player->hasPendingAu = 1;
      }
      int result = decodeAuH264(player->decoder, player->pendingAu.data, player->pendingAu.size, player->pendingAu.pts);
      if (result < 0) return -1;
      if (result == 1) break;            // input queue full: retry this AU later
      player->hasPendingAu = 0;
      fed++;
   }
   return fed;
}

// decodes the next presentation-order frame into `yuv`. returns 1 with dims+pts, 0 at end of
// stream, -1 on error. Feeds access units until the decoder emits a picture, always pulling pictures
// while an AU is outstanding — the decoder can't consume input while its frame buffers are full.
static int decodeNextFrame(VideoPlayer *player, uint8_t *yuv, int *width, int *height, uint64_t *pts, int *startedAtKeyframe)
{
   int stalledMs = 0;
   while (stalledMs < DECODE_STALL_MS && player->running && player->seekRequestNs < 0) {
      int got = getFrameH264(player->decoder, yuv, width, height, pts);
      if (got == 1) return 1;
      if (got < 0) return -1;

      if (player->inputEnded) {   // no more input: drain the pictures still in flight
         if (isH264Drained(player->decoder)) return 0;                       // TEE Decoder: all out
         if (!isTeeH264Decoder(player->decoder) && stalledMs >= END_OF_STREAM_DRAIN_MS) return 0;
         sleepMs(1); stalledMs++;
         continue;
      }

      int fed = feedDecoder(player, startedAtKeyframe);
      if (fed < 0) return -1;
      if (fed > 0) { stalledMs = 0; continue; }
      if (player->inputEnded) continue;
      sleepMs(1); stalledMs++;   // input queue full: give the decoder time, keep pulling pictures
   }
   if (player->running && player->seekRequestNs < 0 && stalledMs >= DECODE_STALL_MS) {
      if (player->inputEnded) return 0;   // the last pictures never came: end anyway
      logError("[video-player] decoder stalled\n");
      return -1;
   }
   return 0;
}

// The TEE Decoder stopped while playing: an error it reported, a stream it cannot take after all (several
// slices per picture only shows while decoding), or no picture for the stall window. Flush it once and go on
// from the next keyframe; a second failure within 30 s, or a failed flush, hands the stream to cellVdec for
// the rest of this run (disableH264TeeForSession). Returns 1 when decoding can go on. The old decoder is closed
// before the next one opens, so their SPEs never add up.
static int recoverDecoder(VideoPlayer *player, int *startedAtKeyframe)
{
   if (!player->decoder || !isTeeH264Decoder(player->decoder) || player->inputEnded) return 0;
   uint64_t now = sys_time_get_system_time();
   int again = player->decoderFailureUs && now - player->decoderFailureUs < 30000000ull;
   player->decoderFailureUs = now;
   player->hasPendingAu = 0;
   *startedAtKeyframe = 0;
   if (!again && resetH264Decoder(player->decoder) == 0) {
      logWarn("[video-player] TEE Decoder flushed after a failure, going on from the next keyframe\n");
      return 1;
   }
   disableH264TeeForSession(again ? "it failed twice within 30 s" : "its flush failed");
   destroyH264Decoder(player->decoder);
   player->decoder = createH264DecoderFor(&player->streamParams);   // cellVdec now
   if (!player->decoder) { logError("[video-player] no decoder after the TEE Decoder failed\n"); return 0; }
   setH264CatchUpSkip(player->decoder, 0);
   logWarn("[video-player] playing on with %s from the next keyframe\n", getH264DecoderName(player->decoder));
   return 1;
}

// flushes both pipelines and jumps to the seek target (decode thread only). The frame on screen
// stays up until the new position's frames arrive.
static void performSeek(VideoPlayer *player)
{
   uint64_t targetNs = (uint64_t)player->seekRequestNs;

   // park the audio thread: it flushes its decoder + the mixer feed, then waits for our release
   if (player->audioAlive) {
      while (player->audioFlushDone && player->audioAlive) sleepMs(1);   // still leaving the previous park
      player->audioFlushRequest = 1;
      while (!player->audioFlushDone && player->audioAlive && player->running) sleepMs(1);
   }
   if (!player->running) return;   // released by teardown, not by a parked audio thread: don't seek a dying player

   // VP9: every picture the decoder does not need on screen goes back first, then the flush. The two on screen
   // (DISPLAYING, RETIRING) stay up until the new position's pictures arrive; their buffers belong to the
   // decoder again after the flush (a release of one from before it is ignored).
   if (player->vp9) {
      // the feeder parks first: it reads the demuxer this seek moves, and the decoder must not be fed while it flushes
      player->vp9FeedParked = 0;
      __sync_synchronize();
      player->vp9FeedPause = 1;
      __sync_synchronize();
      while (player->vp9FeedActive && !player->vp9FeedParked && !player->vp9FeedExited && player->running) sleepMs(1);
      if (!player->running) return;
      lock(&player->lock);
      for (int i = 0; i < FRAME_SLOTS; i++) {
         FrameSlot *slot = &player->slots[i];
         if (slot->state != SLOT_READY && slot->state != SLOT_RELEASE) continue;
         if (slot->hasVp9) tvp9_release_picture(player->vp9, &slot->vp9);
         slot->hasVp9 = 0;
         slot->state = SLOT_EMPTY;
      }
      unlock(&player->lock);
      if (tvp9_flush(player->vp9) != 0) logError("[video-player] TEE VP9 flush failed: %s\n", tvp9_error(player->vp9));
      player->vp9RatePictures = 0;
      player->vp9StartedAtKeyframe = 0;
   }
   // flush the decoder; if the reset wedged it (EndSeq fatal) or it already died, rebuild it rather
   // than going dark (a NULL decoder makes decode report end-of-stream until a later seek revives it)
   else if (!player->decoder || resetH264Decoder(player->decoder) != 0) {
      logWarn("[video-player] decoder reset failed, recreating decoder\n");
      if (isTeeH264Decoder(player->decoder)) disableH264TeeForSession("its flush failed at a seek");
      destroyH264Decoder(player->decoder);
      player->decoder = createH264DecoderFor(&player->streamParams);
      setH264CatchUpSkip(player->decoder, 0);   // fresh decoder, fresh start: re-armed while behind
   }
   player->hasPendingAu = 0;
   player->inputEnded = 0;
   player->vp9FeedError = 0;   // (V3.0.0 Test 12) a seek is a fresh try at the stream
   // land on the fragment keyframe covering the target - or, for a sponsor skip, the next keyframe at/after it
   uint64_t landedNs = seekVideoDemuxer(&player->demuxer, targetNs, player->seekLandAfter);
   // audio follows the video keyframe, but on its own thread: its demuxer seek costs a network
   // reconnect, and doing it here would hold up the first post-seek picture for no reason
   if (player->hasAudioDemuxer) player->audioSeekPendingNs = (int64_t)landedNs;
   logInfo("[video-player] seek to %ds landed at %ds\n", (int)(targetNs / 1000000000ull), (int)(landedNs / 1000000000ull));

   lock(&player->lock);
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_READY) player->slots[i].state = SLOT_EMPTY;
   player->started    = 0;               // the next ready frame re-anchors the clock
   player->ended      = 0;
   player->currentPts = landedNs;
   player->basePts    = landedNs;
   player->audioClockValid = 0;
   player->audioHeld = 0;                // (V3.0.0) the pre-roll below holds and releases the sound itself
   player->audioWaitStartUs = 0;         // fresh pre-roll: each seek gets the full window to wait for audio,
                                         // else a stale timer trips instantly and video starts on the wall clock
   player->statsSecondUs = 0;            // the stutter statistics start over with the new position
   player->statsShown = player->statsLate = player->statsRepeats = player->statsStarved = 0;
   // hold the audio feed until the first post-seek frame actually presents (getVideoFrame releases it), so
   // audio can't drain ahead and then race the video to catch up (the "speedup"). (Test 13) In the same locked step
   // that restarts the pre-roll, so nothing decided before it can start the sound again.
   if (player->audioFeedOpen) pauseAudioPcmFeed(1);
   unlock(&player->lock);

   // the audio thread self-exits at end-of-stream (audioAlive=0, but its decoder/feed/buffers stay intact). a
   // seek repositions the audio demuxer, so respawn the thread to consume the new position - otherwise once
   // the audio stream has ended once, playback stays silent for the rest of the video no matter how you seek.
   if (!player->audioAlive && player->audioThreadActive && (player->aacDecoder || player->opus) && getAudioSource(player)->hasAudio) {
      joinThread(player->audioThread);        // reap the exited thread before reusing its handle
      if (player->aacDecoder) resetAacDecoder(player->aacDecoder);   // (Opus resets at the thread's seek)
      flushAudioPcmFeed();                    // clean feed for the new position (stays paused from above)
      player->audioAlive = 1;                 // set before spawn, mirroring startAudioPipeline's ordering
      __sync_synchronize();
      player->audioThreadActive = (spawnJoinableThread(&player->audioThread, player->opus ? opusAudioThread : audioDecodeThread,
                                   (uint64_t)(uintptr_t)player, THREAD_PRIORITY_DEFAULT,
                                   THREAD_STACK_SIZE_64KB, "video-audio") == 0);
      if (!player->audioThreadActive) {
         player->audioAlive = 0;
         logWarn("[video-player] audio thread respawn failed, staying silent\n");   // also disables later revives
      }
   }

   if (player->seekRequestNs == (int64_t)targetNs) player->seekRequestNs = -1;   // keep a newer request live
   __sync_synchronize();
   player->vp9FeedPause = 0;             // the VP9 feeder reads on from the new position
   player->audioFlushRequest = 0;        // release the audio thread
}

static void decodeThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;
   int startedAtKeyframe = 0;

   while (player->running) {
      if (player->seekRequestNs >= 0) { performSeek(player); startedAtKeyframe = 0; continue; }

      int slot = claimEmptySlot(player);
      if (slot < 0) {   // ring full: wait for the UI to consume a frame
         // ...but keep the TEE Decoder's input topped up meanwhile: fed only when a slot is free, its CABAC lead
         // would shrink to the few pictures the ring holds, and that lead is what carries it over a hard picture
         // (VP9 the same: its entropy stage runs ahead of its pixel stage on what it was fed)
         // (VP9: its feeder thread does that)
         int feedFailed = !player->vp9 && player->decoder && isTeeH264Decoder(player->decoder) && !player->inputEnded &&
                          feedDecoder(player, &startedAtKeyframe) < 0 && !recoverDecoder(player, &startedAtKeyframe);
         if (feedFailed) {
            lock(&player->lock);
            if (player->seekRequestNs < 0) player->ended = 1;
            unlock(&player->lock);
            while (player->running && player->ended && player->seekRequestNs < 0) sleepMs(10);
         }
         sleepMs(2);
         continue;
      }

      int width, height; uint64_t pts;
      int got;
      if (player->vp9) {
         tvp9_picture *pic = &player->slots[slot].vp9;
         got = decodeNextVp9(player, pic);
         if (got == 1) {
            width = pic->width; height = pic->height; pts = (uint64_t)pic->pts;
            player->slots[slot].hasVp9 = 1;   // published with SLOT_READY under the lock below
            afterVp9Picture(player, pic);
         }
      } else {
         got = player->decoder ? decodeNextFrame(player, player->slots[slot].yuv, &width, &height, &pts, &startedAtKeyframe) : -1;
      }
      if (got < 0 && player->running && player->seekRequestNs < 0 && recoverDecoder(player, &startedAtKeyframe)) {
         lock(&player->lock);
         player->slots[slot].state = SLOT_EMPTY;
         unlock(&player->lock);
         continue;
      }

      // Catch-up gate. "Behind" only means something against the clock the picture is paced to -
      // the AU backlog cannot say it, since the decode thread keeps that queue full on purpose.
      if (player->catchUpAllowed && player->started && !player->paused && got == 1) {
         uint64_t clock = presentationClockNs(player, sys_time_get_system_time());
         uint64_t lag   = clock > pts ? clock - pts : 0;
         if (lag > CATCHUP_ENGAGE_NS)       setH264CatchUpSkip(player->decoder, 1);
         else if (lag < CATCHUP_RELEASE_NS) setH264CatchUpSkip(player->decoder, 0);
      }

      lock(&player->lock);
      if (got == 1) {
         player->slots[slot].pts    = pts;
         player->slots[slot].width  = width;
         player->slots[slot].height = height;
         player->slots[slot].state  = SLOT_READY;
         unlock(&player->lock);
      } else {
         player->slots[slot].state = SLOT_EMPTY;
         if (player->seekRequestNs < 0) player->ended = 1;
         unlock(&player->lock);
         // idle at end of stream (or after a hard error) until a seek restarts decode
         while (player->running && player->ended && player->seekRequestNs < 0) sleepMs(10);
      }
   }
   __sync_synchronize();
   player->decodeExited = 1;
   exitThread();
}

// ============================================================================
// audio thread: demuxed AAC frames -> cellAdec -> the mixer's external PCM feed
// ============================================================================

// the demuxer carrying the audio: a separate audio-only source for split (adaptive) streams,
// otherwise the main demuxer (the AAC track muxed alongside the video).
static VideoDemuxer *getAudioSource(VideoPlayer *player)
{
   return player->hasAudioDemuxer ? &player->audioDemuxer : &player->demuxer;
}

static void audioDecodeThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;   // audioAlive is set by startAudioPipeline before we spawn
   int adtsIndex = 0, hasPendingFrame = 0, pendingSize = 0;
   uint64_t pendingPts = 0;
   const uint8_t *pendingFrame = 0;

   while (player->running) {
      // a seek is in progress: flush our pipeline and park until the decode thread releases us
      if (player->audioFlushRequest) {
         resetAacDecoder(player->aacDecoder);
         flushAudioPcmFeed();
         hasPendingFrame = 0;
         player->audioFlushDone = 1;
         while (player->audioFlushRequest && player->running) sleepMs(1);
         player->audioFlushDone = 0;
         continue;
      }

      // reposition our demuxer after a seek (posted by performSeek): the network reconnect this costs
      // runs here, in parallel with the video's own fragment load + decode
      if (player->audioSeekPendingNs >= 0) {
         seekVideoDemuxer(&player->audioDemuxer, (uint64_t)player->audioSeekPendingNs, 0);
         player->audioSeekPendingNs = -1;
         continue;
      }

      // drain decoded PCM into the mixer feed first — the decoder stalls while its buffers are full
      int frames, rate; uint64_t pts;
      int got = getPcmAac(player->aacDecoder, player->pcmScratch, &frames, &rate, &pts);
      if (got < 0) break;
      if (got == 1) {
         if (!player->audioClockValid && !player->ended) {   // anchor the A/V clock to the first audible frame (Test 12: once)
            // the feed and the A/V clock both assume the demuxer's rate; a decoder that disagrees
            // (e.g. SBR doubling the mp4a rate) means wrong pitch AND a drifting clock - make it visible
            int expectedRate = getAudioSource(player)->audioRate;
            if (rate != expectedRate) logWarn("[video-player] decoded audio rate %d Hz != demuxer rate %d Hz\n", rate, expectedRate);
            else logInfo("[video-player] audio up: %d Hz, %d samples/frame\n", rate, frames);
            player->audioClockBasePts = pts;
            __sync_synchronize();
            player->audioClockValid = 1;
         }
         int offset = 0;
         // a park request must break this wait: at start-of-playback the feed is held silent until
         // the first frame presents, a pending seek blocks that present, and the park waits on us -
         // pushing on regardless deadlocked every resume-at-open (decode waits for our park, we wait
         // for feed space, feed space waits for the seek). dropping the remainder is fine: the park's
         // flush discards the whole feed anyway.
         while (offset < frames && player->running && !player->audioFlushRequest) {
            offset += pushAudioPcm(player->pcmScratch + offset * 2, frames - offset);
            if (offset < frames) sleepMs(4);   // feed ring full: the mixer drains ~256 frames per block
         }
         continue;
      }

      // wrap the next demuxed AAC frame in an ADTS header when a scratch buffer is free
      if (!hasPendingFrame) {
         if (getAuBacklogAac(player->aacDecoder) > 1) { sleepMs(1); continue; }
         VideoDemuxer *source = getAudioSource(player);
         AudioAu au;
         // split streams read AAC frames directly (0 = end); a muxed track pops the queue the video
         // thread fills (0 = momentarily empty, more to come)
         int got = player->hasAudioDemuxer ? readAudioAu(source, &au) : takeAudioAu(source, &au);
         if (got <= 0) {
            if (player->hasAudioDemuxer) break;   // split: end of the audio stream
            sleepMs(4); continue;                 // muxed: queue empty for now
         }
         uint8_t *buffer = player->adtsBuffers[adtsIndex];
         if (buildAdtsHeader(buffer, au.size, source->audioAdtsRate, source->audioChannels) != 0) continue;
         memcpy(buffer + ADTS_HEADER_BYTES, au.data, au.size);
         pendingFrame = buffer;
         pendingSize  = au.size + ADTS_HEADER_BYTES;
         pendingPts   = au.pts;
         adtsIndex ^= 1;
         hasPendingFrame = 1;
      }

      int fed = decodeAuAac(player->aacDecoder, pendingFrame, pendingSize, pendingPts);
      if (fed < 0) break;
      if (fed == 0) { hasPendingFrame = 0; continue; }
      sleepMs(1);   // decoder full: loop back to pull PCM
   }
   player->audioAlive = 0;   // video pacing falls back to the wall clock
   exitThread();
}

// (V3.0.0) the audio thread for Opus (an audio-only WebM, YouTube's itag 251 beside VP9): its blocks straight through
// the TEE Opus building block on this PPU thread - no SPE, no backlog: a 20 ms packet decodes in about half a
// millisecond - into the mixer feed. A packet's first sample plays at its time stamp minus the track's CodecDelay;
// after a seek the stream is read from 80 ms before the target (SeekPreRoll) and the decoder drops that run-in.
static void opusAudioThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;   // audioAlive is set by startAudioPipeline before we spawn
   VideoDemuxer *source = getAudioSource(player);
   int64_t delayNs = (int64_t)source->codecDelayNs;
   int64_t skipUntilNs = -1;   // after a seek: packets that end before this are not decoded at all

   while (player->running) {
      if (player->audioFlushRequest) {   // a seek: park until the decode thread lets go
         flushAudioPcmFeed();
         player->audioFlushDone = 1;
         while (player->audioFlushRequest && player->running) sleepMs(1);
         player->audioFlushDone = 0;
         continue;
      }
      if (player->audioSeekPendingNs >= 0) {
         int64_t from = player->audioSeekPendingNs - 80000000ll;   // SeekPreRoll
         if (from < 0) from = 0;
         seekVideoDemuxer(&player->audioDemuxer, (uint64_t)from, 0);
         skipUntilNs = from;
         topus_reset(player->opus, from > 0 ? TOPUS_SEEK_PREROLL : topus_preskip(player->opus));
         player->audioSeekPendingNs = -1;
         continue;
      }

      AudioAu au;
      int got = readAudioAu(source, &au);
      if (got <= 0) break;   // the end of the audio stream (or a read error)
      int packetFrames = topus_packet_frames(au.data, (size_t)au.size);
      if (skipUntilNs >= 0) {   // the demuxer lands on the cue before the target: read on to the run-in's start
         int64_t endNs = (int64_t)au.pts + (int64_t)(packetFrames > 0 ? packetFrames : 960) * 1000000000ll / 48000;
         if (endNs < skipUntilNs) continue;
         skipUntilNs = -1;
      }
      int frames = topus_decode(player->opus, au.data, (size_t)au.size, player->pcmScratch);
      if (frames <= 0) continue;   // pre-skip or run-in being dropped, or a packet that is not Opus

      // anchor the A/V clock to the first audible frame - not again once the picture has ended: the presenter then
      // drops the audio clock with every frame, and the anchor came back with every packet (Test 11: "audio up" 50
      // times a second, the clock jumping)
      if (!player->audioClockValid && !player->ended) {
         int dropped = packetFrames > frames ? packetFrames - frames : 0;
         int64_t firstNs = (int64_t)au.pts - delayNs + (int64_t)dropped * 1000000000ll / 48000;
         player->audioClockBasePts = firstNs < 0 ? 0 : (uint64_t)firstNs;
         __sync_synchronize();
         player->audioClockValid = 1;
         logInfo("[video-player] audio up: Opus 48000 Hz, %d samples/packet, first sample at %d ms\n", packetFrames,
                 (int)(player->audioClockBasePts / 1000000ull));
      }
      int offset = 0;
      while (offset < frames && player->running && !player->audioFlushRequest) {   // as the AAC thread does
         offset += pushAudioPcm(player->pcmScratch + offset * 2, frames - offset);
         if (offset < frames) sleepMs(4);
      }
   }
   player->audioAlive = 0;   // video pacing falls back to the wall clock
   exitThread();
}

// tears down whatever part of the audio pipeline came up (also the failure path during create)
static void releaseAudioPipeline(VideoPlayer *player)
{
   if (player->audioThreadActive) { joinThread(player->audioThread); player->audioThreadActive = 0; }
   if (player->audioFeedOpen) { closeAudioPcmFeed(); player->audioFeedOpen = 0; }
   destroyAacDecoder(player->aacDecoder);
   player->aacDecoder = 0;
   if (player->opus) { topus_close(player->opus); player->opus = 0; }
   for (int i = 0; i < 2; i++) { free(player->adtsBuffers[i]); player->adtsBuffers[i] = 0; }
   free(player->pcmScratch);
   player->pcmScratch = 0;
}

// brings up the AAC decoder + mixer feed + audio thread; on any failure the video plays silent.
static void startAudioPipeline(VideoPlayer *player)
{
   VideoDemuxer *source = getAudioSource(player);
   if (!source->hasAudio) return;

   if (source->isOpus) {   // (V3.0.0) Opus on the PPU: VP9's sound, with every SPE on the picture
      char err[160];
      err[0] = 0;
      player->opus = topus_open(source->opusHead, (size_t)source->opusHeadSize, err, sizeof err);
      player->pcmScratch = (float *)malloc(TOPUS_MAX_FRAMES * 2 * sizeof(float));
      player->audioFeedOpen = openAudioPcmFeed(TOPUS_RATE, 0) == 0;
      if (!player->opus) logWarn("[video-player] TEE Opus did not open: %s\n", err);
      if (!player->opus || !player->pcmScratch || !player->audioFeedOpen) goto silent;
      logInfo("[video-player] audio: Opus on the PPU (TEE Opus %s), %d channel(s), pre-skip %d\n", TEE_OPUS_VERSION,
              topus_channels(player->opus), topus_preskip(player->opus));
      player->audioAlive = 1;
      player->audioThreadActive = (spawnJoinableThread(&player->audioThread, opusAudioThread, (uint64_t)(uintptr_t)player,
                                   THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-opus") == 0);
      if (!player->audioThreadActive) goto silent;
      return;
   }

   player->aacDecoder = createAacDecoder();
   player->adtsBuffers[0] = (uint8_t *)malloc(ADTS_HEADER_BYTES + AUDIO_AU_MAX_BYTES);
   player->adtsBuffers[1] = (uint8_t *)malloc(ADTS_HEADER_BYTES + AUDIO_AU_MAX_BYTES);
   player->pcmScratch = (float *)malloc(AAC_MAX_FRAME_SAMPLES * 2 * sizeof(float));
   player->audioFeedOpen = openAudioPcmFeed(source->audioRate, 0) == 0;   // 0 = the default file-playback backlog

   if (!player->aacDecoder || !player->adtsBuffers[0] || !player->adtsBuffers[1] || !player->pcmScratch || !player->audioFeedOpen) goto silent;

   // mark alive before the thread starts: the resume seek is posted right after open, and performSeek
   // must park (not race) the audio thread's first demuxer read. the thread clears this on exit.
   player->audioAlive = 1;
   player->audioThreadActive = (spawnJoinableThread(&player->audioThread, audioDecodeThread, (uint64_t)(uintptr_t)player,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-audio") == 0);
   if (!player->audioThreadActive) goto silent;
   return;

silent:
   player->audioAlive = 0;
   logWarn("[video-player] audio pipeline failed to start, playing silent\n");
   releaseAudioPipeline(player);
}

// ============================================================================
// lifecycle
// ============================================================================

// opens the split audio demuxer on a side thread, in parallel with the video open (each open costs
// an https connect + header parse, so doing them together nearly halves startup). create joins this
// before anything touches the audio demuxer.
typedef struct { VideoPlayer *player; const char *path; int ok; } AudioOpenJob;

static void audioOpenThread(uint64_t arg)
{
   AudioOpenJob *job = (AudioOpenJob *)(uintptr_t)arg;
   job->ok = openAudioDemuxer(&job->player->audioDemuxer, job->path) == 0;
   exitThread();
}

static int roundUp16(int value) { return (value + 15) & ~15; }

// the screen's refresh rate as the console drives it, 0 if unknown
static double screenRefreshHz(void)
{
   CellVideoOutState state;
   if (cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &state) < 0) return 0.0;
   uint16_t rates = state.displayMode.refreshRates;
   if (rates & CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ) return 60000.0 / 1001.0;
   if (rates & CELL_VIDEO_OUT_REFRESH_RATE_60HZ)    return 60.0;
   if (rates & CELL_VIDEO_OUT_REFRESH_RATE_50HZ)    return 50.0;
   if (rates & CELL_VIDEO_OUT_REFRESH_RATE_30HZ)    return 30.0;
   return 0.0;
}

// display sync when the video's frame rate is the screen's (or half of it) to within 1%: then one picture per
// vblank (or per two) plays it at the screen's pace, and the sound follows at screen rate / (n x frame rate)
static void setUpDisplaySync(VideoPlayer *player)
{
   double screenHz = screenRefreshHz();
   if (screenHz <= 0.0 || !player->demuxer.frameDurationNs) {
      logInfo("[video-player] display sync off: screen rate %s, frame rate %s\n", screenHz > 0.0 ? "known" : "unknown",
              player->demuxer.frameDurationNs ? "known" : "unknown");
      return;
   }
   double fps = 1.0e9 / (double)player->demuxer.frameDurationNs;
   for (int perFrame = 1; perFrame <= 2 && !player->syncPerFrame; perFrame++) {
      double ratio = screenHz / ((double)perFrame * fps);
      if (ratio > 0.99 && ratio < 1.01) { player->syncPerFrame = perFrame; player->syncBaseSpeed = ratio; }
   }
   player->syncVblankUs = (uint64_t)(1.0e6 / screenHz + 0.5);
   // integers only: this printf has no floating point
   if (player->syncPerFrame)
      logInfo("[video-player] display sync on: screen %d mHz, video %d mHz, %d vblank(s) a picture, sound tempo %d ppm\n",
              (int)(screenHz * 1000.0 + 0.5), (int)(fps * 1000.0 + 0.5), player->syncPerFrame,
              (int)(player->syncBaseSpeed * 1.0e6 + 0.5));
   else
      logInfo("[video-player] display sync off: screen %d mHz, video %d mHz\n", (int)(screenHz * 1000.0 + 0.5),
              (int)(fps * 1000.0 + 0.5));
}

void setVideoVp9Spes(int spes) { vp9Spes = spes == 5 ? 5 : 6; }

int isVideoVp9(const VideoPlayer *player) { return player && player->vp9; }
int isAudioOpus(const VideoPlayer *player) { return player && player->opus; }

static void logVp9Line(const char *line) { logInfo("[tee-vp9] %s\n", line); }

// VP9: opens the TEE VP9 building block for the stream (createVideoPlayerSplit, before the decode thread). Its
// picture buffers come from the UI's allocator - RSX-visible, 1 MB aligned - so the RSX draws the pictures where
// the decoder wrote them; no H.264 frame slots are allocated. With all 6 SPEs on the picture the sound is closed
// (cellAdec would be a 7th, and a 7th SPU thread froze the console). 0 ok, -1 failed.
static int openVp9(VideoPlayer *player, VideoFrameAllocFn allocFrame)
{
   tvp9_config config;
   tvp9_default(&config);
   config.log = logVp9Line;
   if (vp9Spes == 5) { config.entropy_spes = 1; config.pixel_spes = 4; }
   // buffers for the stream's size, not the module's 2560 x 1440 default: a 1080p stream needs less memory
   if (player->demuxer.width > 0 && player->demuxer.height > 0 &&
       player->demuxer.width <= config.max_width && player->demuxer.height <= config.max_height) {
      config.max_width  = player->demuxer.width;
      config.max_height = player->demuxer.height;
   }
   char err[160];
   err[0] = 0;
   int pictures = 0;
   for (unsigned setup = 0; setup < sizeof VP9_SETUPS / sizeof VP9_SETUPS[0] && !player->vp9; setup++) {
      pictures = VP9_SETUPS[setup].pictures;
      config.pictures = pictures;
      config.input_bytes = VP9_SETUPS[setup].inputMb << 20;
      size_t bytes = tvp9_picture_memory_size(&config);
      player->vp9Memory = allocFrame(bytes);
      if (!player->vp9Memory || ((uintptr_t)player->vp9Memory & 0xFFFFF)) {
         snprintf(err, sizeof err, "no 1 MB aligned block of %d MB for %d pictures", (int)(bytes >> 20), pictures);
         if (player->vp9Memory) player->freeFrame(player->vp9Memory);
         player->vp9Memory = 0;
         continue;
      }
      config.picture_memory = player->vp9Memory;
      config.picture_memory_bytes = bytes;
      player->vp9 = tvp9_open(&config, err, sizeof err);
      if (!player->vp9) { player->freeFrame(player->vp9Memory); player->vp9Memory = 0; }
      else logInfo("[video-player] TEE VP9 %s: %dx%d stream, %d + %d SPEs, %d pictures, %d MB input (%d MB + %d MB)%s\n",
                   TEE_VP9_VERSION, player->demuxer.width, player->demuxer.height, config.entropy_spes, config.pixel_spes,
                   pictures, (int)(config.input_bytes >> 20), (int)(bytes >> 20), (int)(tvp9_memory_size(&config) >> 20),
                   player->hasAudioDemuxer && player->audioDemuxer.isOpus ? ", Opus sound on the PPU"
                   : vp9Spes == 6 ? ", no sound (AAC needs an SPE)" : ", AAC sound");
   }
   sys_memory_info_t memory;
   if (player->vp9 && sys_memory_get_user_memory_size(&memory) == CELL_OK)
      logInfo("[video-player] user memory with TEE VP9 open: %u MB free of %u MB (heap not counted)\n",
              (unsigned)(memory.available_user_memory >> 20), (unsigned)(memory.total_user_memory >> 20));
   if (!player->vp9) { logError("[video-player] TEE VP9 did not open: %s\n", err); return -1; }
   if (vp9Spes == 6 && player->hasAudioDemuxer && !player->audioDemuxer.isOpus) {   // AAC: no SPE left for cellAdec
      closeVideoDemuxer(&player->audioDemuxer);
      player->hasAudioDemuxer = 0;
   }
   return 0;
}

// heap fallbacks when the caller supplies no frame allocator
static void *allocFrameFromHeap(size_t size) { return memalign(128, size); }
static void freeFrameToHeap(void *buffer) { free(buffer); }

VideoPlayer *createVideoPlayer(const char *path, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame)
{
   return createVideoPlayerSplit(path, 0, allocFrame, freeFrame);
}

VideoPlayer *createVideoPlayerSplit(const char *videoPath, const char *audioPath, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame)
{
   VideoPlayer *player = (VideoPlayer *)calloc(1, sizeof *player);
   if (!player) return 0;
   player->displayIndex = -1;
   player->seekRequestNs = -1;
   player->audioSeekPendingNs = -1;
   player->freeFrame = freeFrame ? freeFrame : freeFrameToHeap;
   if (!allocFrame) allocFrame = allocFrameFromHeap;
   createLock(&player->lock);

   // adaptive streams carry audio in a separate url; open it in parallel with the video open below.
   // failure just means silent playback, so it never fails the whole player.
   AudioOpenJob audioJob = { player, audioPath, 0 };
   sys_ppu_thread_t audioOpenTid;
   int audioOpening = audioPath && audioPath[0] &&
                      spawnJoinableThread(&audioOpenTid, audioOpenThread, (uint64_t)(uintptr_t)&audioJob,
                                          THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "audio-open") == 0;

   int videoOk = openVideoDemuxer(&player->demuxer, videoPath) == 0;

   // join the audio open before ANY exit: the thread writes into the player, so it must be done
   // (and its outcome adopted) before a failed video open can tear the player down
   if (audioOpening) { joinThread(audioOpenTid); player->hasAudioDemuxer = audioJob.ok; }
   else if (audioPath && audioPath[0]) player->hasAudioDemuxer = openAudioDemuxer(&player->audioDemuxer, audioPath) == 0;
   if (audioPath && audioPath[0] && !player->hasAudioDemuxer)
      logWarn("[video-player] separate audio stream failed to open, playing silent\n");

   if (!videoOk) goto fail;

   if (player->demuxer.isVp9) {
      if (openVp9(player, allocFrame) != 0) goto fail;
      goto decoderReady;
   }

   // the decoder must be built for the stream's CODED size, which the SPS alone knows: the container
   // reports the DISPLAY size, and encoders pad beyond it (Intel QuickSync codes 720 as 736 and crops
   // it back). Get it wrong and cellVdec either decodes nothing at all or locks the console, so read
   // it from the SPS the demuxer already extracted, and only guess if there isn't one.
   H264StreamInfo streamInfo;
   if (readH264StreamInfo(player->demuxer.h264.header, player->demuxer.h264.headerSize, &streamInfo) == 0) {
      player->allocW = streamInfo.codedWidth;
      player->allocH = streamInfo.codedHeight;
      if (player->allocW != player->demuxer.width || player->allocH != player->demuxer.height)
         logInfo("[video-player] stream displays %dx%d but codes %dx%d\n", player->demuxer.width, player->demuxer.height,
                 player->allocW, player->allocH);
   } else {
      logWarn("[video-player] no SPS in the container header, guessing the coded size from the display size\n");
      player->allocW = roundUp16(player->demuxer.width);
      player->allocH = roundUp16(player->demuxer.height);
   }
   // YUV 4:2:0 planar frames; cellVdecGetPicture requires 128-byte alignment, size a multiple of 128. Allocated
   // before the decoder, so the decoder's memory check sees what is really left once they exist.
   size_t frameBytes = ((size_t)player->allocW * player->allocH * 3 / 2 + 127) & ~(size_t)127;
   for (int i = 0; i < FRAME_SLOTS; i++) {
      player->slots[i].yuv = (uint8_t *)allocFrame(frameBytes);
      if (!player->slots[i].yuv) goto fail;
   }

   // the TEE Decoder where it can take the stream (and the app allows it, see setH264DecoderPolicy), cellVdec
   // otherwise - decode-h264.c reads the stream's own SPS and PPS to decide
   player->streamParams.width        = player->allocW;
   player->streamParams.height       = player->allocH;
   player->streamParams.level        = player->demuxer.level;
   player->streamParams.maxRefFrames = player->demuxer.h264.maxRefFrames;
   player->streamParams.header       = player->demuxer.h264.header;
   player->streamParams.headerSize   = player->demuxer.h264.headerSize;
   player->streamParams.spsPatched   = player->demuxer.h264.spsPatched;
   player->decoder = createH264DecoderFor(&player->streamParams);
   if (!player->decoder) goto fail;

   // 1080p decode tops out around 35fps on cellVdec's SPUs. A blanket B_SKIP is the wrong answer - these
   // encodes are ~87% disposable B-frames, so dropping them all leaves a ~7fps slideshow - but
   // letting the decoder drop them ONLY while it is behind buys back real time on the anchor-heavy
   // encodes (live, where there is little to lose) and leaves everything it already keeps up with
   // decoded picture for picture. The TEE Decoder keeps up with 1080p60, but the same net stays under it.
   if (player->demuxer.frameDurationNs && player->demuxer.frameDurationNs < 32000000ull) {
      logWarn("[video-player] %d fps on %s, catch-up skip enabled\n",
              (int)(1000000000ull / player->demuxer.frameDurationNs), getH264DecoderName(player->decoder));
      player->catchUpAllowed = 1;   // armed; the decode thread engages it only while behind
   }
decoderReady:
   setUpDisplaySync(player);

   player->running = 1;
   player->threadActive = (spawnJoinableThread(&player->thread, decodeThread, (uint64_t)(uintptr_t)player,
                           THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-player") == 0);
   if (!player->threadActive) goto fail;
   if (player->vp9) {
      player->vp9FeedActive = (spawnJoinableThread(&player->vp9FeedTid, vp9FeedThread, (uint64_t)(uintptr_t)player,
                               THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "vp9-feed") == 0);
      if (!player->vp9FeedActive) goto fail;
   }

   startAudioPipeline(player);
   // hold audio silent until the video pre-roll releases: otherwise audio drains during the ~0.7s wait
   // and its clock runs ahead, so video skips to catch up at the start (the "fast start"). getVideoFrame
   // resumes the feed at the first present, so audio and video begin together. Audio buffered meanwhile
   // just fills the feed ring, giving a head start on resume.
   if (player->audioFeedOpen) pauseAudioPcmFeed(1);
   return player;

fail:
   destroyVideoPlayer(player);
   return 0;
}

#define DESTROY_WAIT_MS 5000   // how long teardown waits for the decode thread to leave

void destroyVideoPlayer(VideoPlayer *player)
{
   if (!player) return;
   player->running = 0;
   cancelVideoDemuxer(&player->demuxer);                                  // unblock a live read waiting on the edge
   if (player->hasAudioDemuxer) cancelVideoDemuxer(&player->audioDemuxer);
   if (player->threadActive) {
      // The decode thread leaves within a frame or two once running is down and the reads are cancelled - unless it
      // is stuck in the TEE Decoder (tdec_flush after a failure or at a seek, when an SPE never stops: the module's
      // known hang). Waiting on it froze the whole app. After DESTROY_WAIT_MS the player is left to that thread:
      // only the sound goes (the next player needs the mixer feed), everything else stays allocated, as the
      // thread may still use it, and the TEE Decoder stays off for the rest of the run.
      for (int waited = 0; (!player->decodeExited || (player->vp9FeedActive && !player->vp9FeedExited)) &&
                           waited < DESTROY_WAIT_MS; waited += 10) sleepMs(10);
      if (!player->decodeExited || (player->vp9FeedActive && !player->vp9FeedExited)) {
         logError("[video-player] the decode thread did not stop in %d ms (the TEE Decoder hangs?): player left behind\n",
                  DESTROY_WAIT_MS);
         disableH264TeeForSession("it hung");
         releaseAudioPipeline(player);
         return;
      }
      joinThread(player->thread);
      if (player->vp9FeedActive) joinThread(player->vp9FeedTid);
   }
   releaseAudioPipeline(player);
   destroyH264Decoder(player->decoder);
   if (player->vp9) tvp9_close(player->vp9);                         // the SPEs stop (by force if one hangs)
   if (player->vp9Memory) player->freeFrame(player->vp9Memory);
   closeVideoDemuxer(&player->demuxer);
   if (player->hasAudioDemuxer) closeVideoDemuxer(&player->audioDemuxer);
   for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].yuv) player->freeFrame(player->slots[i].yuv);
   destroyLock(&player->lock);
   free(player);
}

// ============================================================================
// presentation
// ============================================================================

// the frame currently on screen (NULL before the first-ever present). shown while a seek or the
// post-seek pre-roll refills, so playback cuts frame-to-frame instead of dropping to black. the
// DISPLAYING slot survives performSeek's flush (only READY slots are emptied), so it stays valid.
// caller holds player->lock; this releases it.
static const uint8_t *takeHeldFrame(VideoPlayer *player, int *width, int *height)
{
   int held = player->displayIndex;
   const uint8_t *heldBuffer = 0;
   if (held >= 0) { heldBuffer = slotPicture(&player->slots[held]); *width = player->slots[held].width; *height = player->slots[held].height; }
   unlock(&player->lock);
   return heldBuffer;
}

#define SYNC_CORRECTION_NS  10000000000ll   // the sound's tempo moves by error / 10 s ...
#define SYNC_MAX_CORRECTION 0.005           // ... by 0.5% at most (8.6 cents, and only while it closes a gap)

// Display sync (caller holds player->lock): the picture advances by the vblanks that went by since the last
// present - one picture per vblank, or per two - not by the clock, so the beat between a 60 fps video and a
// 59.94 Hz screen never makes it skip. The sound's tempo is steered so the clock stays with the picture (see
// getVideoFrame). Only a real gap goes back to dropping or holding: the picture more than 1.5 pictures behind
// the clock skips one, more than 1.5 ahead waits. Returns the slot to show, -1 to keep the current one.
static int pickSyncedFrame(VideoPlayer *player, uint64_t nowUs, uint64_t clock)
{
   int64_t vblankUs = (int64_t)player->syncVblankUs;
   int elapsed = (int)(((int64_t)(nowUs - player->syncLastUs) + vblankUs / 2) / vblankUs);
   if (elapsed < 0) elapsed = 0;
   player->syncLastUs = nowUs;
   player->syncVblanks += elapsed;
   int advance = player->syncVblanks / player->syncPerFrame;
   player->syncVblanks -= advance * player->syncPerFrame;

   int64_t frameNs = (int64_t)player->demuxer.frameDurationNs;
   int64_t errorNs = (int64_t)player->currentPts - (int64_t)clock;   // > 0: the picture is ahead of the sound
   int best = -1;
   if (errorNs < -frameNs * 6) {   // far behind (a stall): straight to the picture that is due, as without sync
      uint64_t bestPts = 0;
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY && player->slots[i].pts <= clock && (best < 0 || player->slots[i].pts >= bestPts)) { best = i; bestPts = player->slots[i].pts; }
      return best;
   }
   if (errorNs < -frameNs * 3 / 2) advance++;                        // behind: skip one picture to catch up
   else if (errorNs > frameNs * 3 / 2) advance = 0;                  // ahead: hold this one

   uint64_t after = player->currentPts;
   for (int step = 0; step < advance; step++) {
      int next = -1;
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY && player->slots[i].pts > after &&
             (next < 0 || player->slots[i].pts < player->slots[next].pts)) next = i;
      if (next < 0) break;   // nothing newer decoded yet: show what is there, or repeat
      best = next;
      after = player->slots[next].pts;
   }
   return best;
}

const uint8_t *getVideoFrame(VideoPlayer *player, int *width, int *height)
{
   uint64_t nowUs = sys_time_get_system_time();
   int releaseAudio = 0;   // set when pre-roll fires, to un-hold the audio feed after the lock is dropped
   char prerollLine[160];  // (V3.0.0 Test 13) where picture and sound stand when the pre-roll ends
   prerollLine[0] = 0;
   lock(&player->lock);

   // a posted seek freezes presentation: frames decoded before the flush must not advance the
   // position; the displayed frame stays up until the new position's frames arrive
   if (player->seekRequestNs >= 0) return takeHeldFrame(player, width, height);

   // anchor the clock to the earliest ready frame, with a short pre-roll: hold the first present until a
   // few frames are buffered (PREROLL_FRAMES) and the audio clock is live. Starting on the very first
   // decoded frame began playback with drops (the clock outran the cold decoder) and a visible stutter
   // when the audio clock later took over from the wall clock with a backward step.
   if (!player->started) {
      uint64_t earliest = 0; int found = -1, readyCount = 0;
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY) { readyCount++; if (found < 0 || player->slots[i].pts < earliest) { earliest = player->slots[i].pts; found = i; } }
      int decoderWarm  = readyCount >= (player->vp9 ? VP9_PREROLL_FRAMES : PREROLL_FRAMES) || player->ended;   // a short clip may never fill the ring
      int audioWaiting = player->audioAlive && !player->audioClockValid;     // a dead/absent audio pipeline never holds video

      // don't wait forever for audio to anchor the clock: on a stalled audio stream it never decodes a
      // frame, audioClockValid never sets, and video would hang on a black screen (a "lockup"). After a
      // few seconds, start video without it rather than never starting.
      if (audioWaiting) {
         if (!player->audioWaitStartUs) player->audioWaitStartUs = nowUs;
         else if (nowUs - player->audioWaitStartUs > PREROLL_AUDIO_TIMEOUT_US) {
            logWarn("[video-player] audio clock never anchored, starting video without it\n");
            audioWaiting = 0;
         }
      }
      if (found < 0 || !decoderWarm || audioWaiting) return takeHeldFrame(player, width, height);
      player->basePts    = earliest;
      player->baseWallUs = nowUs;
      player->started    = 1;
      if (player->vp9)
         snprintf(prerollLine, sizeof prerollLine, "[video-player] pre-roll done: picture at %d ms, sound at %d ms (%d frames "
                  "played), %d ready%s\n", (int)(earliest / 1000000ull),
                  player->audioClockValid ? (int)(audioClockNs(player) / 1000000ull) : -1,
                  (int)getAudioPcmFeedPlayedFrames(), readyCount, player->paused ? ", paused" : "");
      // start the audio feed together with the first video frame - unless the video is paused: a seek while paused
      // ended its pre-roll here and let the sound run on under a still picture; X then "paused" it by playing the
      // picture, which caught up seconds of video without sound (TEE, Test 12). Unpausing starts the feed.
      releaseAudio       = !player->paused;
   }

   // the audio clock is the sync master while audio runs: it only advances as samples reach the
   // speakers, so video locked to it cannot drift. When audio ends or dies, re-anchor the wall clock
   // at the current position and pace on that instead.
   if (player->audioClockValid && (!player->audioAlive || player->ended)) {
      player->audioClockValid = 0;
      player->basePts    = player->currentPts;
      player->baseWallUs = nowUs;
   }

   steerPresentationClock(player, nowUs);
   uint64_t clock = presentationClockNs(player, nowUs);

   int best = -1, readyCount = 0; uint64_t bestPts = 0;
   for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].state == SLOT_READY) readyCount++;

   // display sync while the sound runs (see pickSyncedFrame); it locks on from a picture chosen by the clock
   int synced = player->syncPerFrame && !player->paused &&
                ((player->audioClockValid && player->audioFeedOpen) || (player->vp9 && !player->audioFeedOpen));
   if (synced && player->syncActive) {
      best = pickSyncedFrame(player, nowUs, clock);
      if (best >= 0) bestPts = player->slots[best].pts;
   } else {
      // pick the latest ready frame whose pts is due; drop older ready frames (skip if we fell behind)
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY && player->slots[i].pts <= clock && (best < 0 || player->slots[i].pts >= bestPts)) { best = i; bestPts = player->slots[i].pts; }
      if (synced && best >= 0) { player->syncActive = 1; player->syncLastUs = nowUs; player->syncVblanks = 0; }
   }
   if (!synced) player->syncActive = 0;

   int late = 0;
   if (best >= 0) {
      for (int i = 0; i < FRAME_SLOTS; i++) {
         if (player->slots[i].state == SLOT_RETIRING) retireSlot(player, i);                             // GPU is done with it by now
         else if (player->slots[i].state == SLOT_DISPLAYING) player->slots[i].state = SLOT_RETIRING;      // hold one present for the frame in flight
         else if (player->slots[i].state == SLOT_READY && player->slots[i].pts < bestPts) { retireSlot(player, i); late++; }   // drop skipped
      }
      player->slots[best].state = SLOT_DISPLAYING;
      player->displayIndex = best;
      player->currentPts   = bestPts;
   }

   // display sync: the sound's tempo keeps the clock with the picture - the screen's rate over the video's, plus a
   // correction that closes the gap between them (picture ahead: the sound a hair faster). Back to 1.0 otherwise.
   char syncLine[160];
   syncLine[0] = 0;
   if (synced && player->syncActive) {
      int64_t errorNs = (int64_t)player->currentPts - (int64_t)clock;
      double correction = (double)errorNs / (double)SYNC_CORRECTION_NS;
      if (correction >  SYNC_MAX_CORRECTION) correction =  SYNC_MAX_CORRECTION;
      if (correction < -SYNC_MAX_CORRECTION) correction = -SYNC_MAX_CORRECTION;
      double speed = player->syncBaseSpeed * (1.0 + correction);
      if (player->audioFeedOpen) {   // (VP9 without sound: nothing to steer, the clock is the screen's - see presentationClockNs)
         setAudioPcmFeedSpeed(speed);
         player->syncSpeedSet = 1;
      }
      if (nowUs - player->syncLogUs >= 10000000ull) {
         player->syncLogUs = nowUs;
         if (player->audioFeedOpen)
            snprintf(syncLine, sizeof syncLine, "[video-player] display sync: picture %d ms %s the sound, sound tempo %d ppm\n",
                     (int)((errorNs < 0 ? -errorNs : errorNs) / 1000000), errorNs < 0 ? "behind" : "ahead of",
                     (int)(speed * 1.0e6 + 0.5));
         else
            snprintf(syncLine, sizeof syncLine, "[video-player] display sync without sound: picture %d ms %s the clock\n",
                     (int)((errorNs < 0 ? -errorNs : errorNs) / 1000000), errorNs < 0 ? "behind" : "ahead of");
      }
   } else if (player->syncSpeedSet) {
      setAudioPcmFeedSpeed(1.0);
      player->syncSpeedSet = 0;
   }

   // (V3.0.0) VP9: the sound waits for a picture that fell behind it (see audioHeld). Decided here, done after the
   // lock is dropped (mixer calls must run outside player->lock).
   int holdAction = 0;   // 1: pause the sound, 2: let it run on
   char holdLine[128];
   holdLine[0] = 0;
   if (player->audioHeld && (player->paused || !player->vp9 || !player->audioFeedOpen || !player->audioClockValid)) {
      player->audioHeld = 0;   // the user's pause owns the feed now; at the end of the sound, what is left plays out
      if (!player->paused) holdAction = 2;
   } else if (player->vp9 && player->audioFeedOpen && player->audioClockValid && player->started && !player->paused) {
      int64_t behindNs = (int64_t)clock - (int64_t)player->currentPts;   // > 0: the picture is behind the sound
      int readyAfter = 0;
      for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].state == SLOT_READY) readyAfter++;
      if (!player->audioHeld) {
         if (behindNs > AV_HOLD_ENGAGE_NS && !player->ended) {
            player->audioHeld = 1;
            player->audioHeldSinceUs = nowUs;
            holdAction = 1;
            snprintf(holdLine, sizeof holdLine, "[video-player] the sound waits for the picture: %d ms behind it, %d decoded\n",
                     (int)(behindNs / 1000000), readyAfter);
         }
      } else {
         uint64_t heldUs = nowUs - player->audioHeldSinceUs;
         if (player->ended || (behindNs <= 0 && (readyAfter >= AV_HOLD_READY || heldUs >= AV_HOLD_MAX_US))) {
            player->audioHeld = 0;
            holdAction = 2;
            snprintf(holdLine, sizeof holdLine, "[video-player] the sound runs on after %d ms, %d pictures decoded ahead\n",
                     (int)(heldUs / 1000), readyAfter);
         }
      }
   }

   // the stutter statistics: a present with a new picture, pictures dropped as late, a present with none due,
   // and none ready at all (the decoder behind, not just early)
   char statsLine[200];
   statsLine[0] = 0;
   if (!player->paused) {
      if (!player->statsSecondUs) { player->statsSecondUs = nowUs; player->statsReadyMin = FRAME_SLOTS; }
      if (best >= 0) player->statsShown++; else player->statsRepeats++;
      if (best < 0 && readyCount == 0) player->statsStarved++;
      player->statsLate += late;
      if (readyCount < player->statsReadyMin) player->statsReadyMin = readyCount;
      if (nowUs - player->statsSecondUs >= 1000000ull) {
         if (player->statsLate || player->statsStarved)
            snprintf(statsLine, sizeof statsLine, "[video-player] 1 s: %d pictures shown, %d dropped as late, %d presents "
                     "without a new one (%d with none ready), fewest ready %d of %d\n", player->statsShown, player->statsLate,
                     player->statsRepeats, player->statsStarved, player->statsReadyMin, FRAME_SLOTS);
         player->statsSecondUs = nowUs;
         player->statsShown = player->statsLate = player->statsRepeats = player->statsStarved = 0;
         player->statsReadyMin = FRAME_SLOTS;
      }
   }

   int idx = player->displayIndex;
   const uint8_t *buffer = 0;
   if (idx >= 0) { buffer = slotPicture(&player->slots[idx]); *width = player->slots[idx].width; *height = player->slots[idx].height; }
   // release the audio held through pre-roll, and the VP9 hold. (V3.0.0 Test 13) Under the lock, where they are
   // decided: done after it, a seek the decode thread made in between (it pauses the sound for its pre-roll) was undone
   // by them, and the sound ran under the old picture until the new one came. pauseAudioPcmFeed only sets a flag.
   if (releaseAudio && player->audioFeedOpen) pauseAudioPcmFeed(0);
   if (holdAction && player->audioFeedOpen) pauseAudioPcmFeed(holdAction == 1);
   unlock(&player->lock);
   if (statsLine[0]) logInfo("%s", statsLine);   // outside the lock: a file write must not hold up the decode thread
   if (syncLine[0]) logInfo("%s", syncLine);
   if (holdLine[0]) logInfo("%s", holdLine);
   if (prerollLine[0]) logInfo("%s", prerollLine);
   return buffer;
}

void setVideoPaused(VideoPlayer *player, int paused)
{
   uint64_t nowUs = sys_time_get_system_time();
   lock(&player->lock);
   if (paused && !player->paused) {
      player->paused = 1;
      player->pauseWallUs = nowUs;
   } else if (!paused && player->paused) {
      player->paused = 0;
      player->baseWallUs += nowUs - player->pauseWallUs;   // don't count paused time against the clock
      player->clockSmoothValid = 0;                        // the smoothed clock picks the audio up afresh
   }
   // (V3.0.0 Test 13) X during a pre-roll (a seek's pictures still coming) leaves the sound to the pre-roll, which starts
   // it with the first picture: started at once, it ran under the old picture and the new one came seconds behind it.
   // Held by the VP9 hold, it stays held until the picture has caught up.
   if (player->audioFeedOpen) {
      if (paused) pauseAudioPcmFeed(1);
      else if (player->started && !player->audioHeld && player->seekRequestNs < 0) pauseAudioPcmFeed(0);
   }
   unlock(&player->lock);
}

int isVideoPaused(const VideoPlayer *player) { return player->paused; }

// post a seek target for the decode thread. landAfter=1 rounds up to the next keyframe at/after `seconds`
// (used to skip fully past a sponsor segment); landAfter=0 lands on the keyframe covering it (normal scrub).
static void postSeek(VideoPlayer *player, float seconds, int landAfter)
{
   if (seconds < 0.0f) seconds = 0.0f;
   float duration = getVideoDurationSeconds(player);
   if (duration > 0.0f && seconds > duration) seconds = duration;
   int64_t targetNs = (int64_t)(seconds * 1.0e9f);
   lock(&player->lock);
   player->currentPts = (uint64_t)targetNs;   // position reads as the target until the seek lands
   unlock(&player->lock);
   player->seekLandAfter = landAfter;
   __sync_synchronize();                       // publish the land direction before the decode thread sees the request
   player->seekRequestNs = targetNs;
}

void seekVideoPlayer(VideoPlayer *player, float seconds)     { postSeek(player, seconds, 0); }
void seekVideoPlayerPast(VideoPlayer *player, float seconds) { postSeek(player, seconds, 1); }

int isVideoEnded(const VideoPlayer *player)
{
   if (!player->ended) return 0;
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_READY || player->slots[i].state == SLOT_FILLING) return 0;
   return 1;
}

float getVideoPositionSeconds(const VideoPlayer *player)
{
   return (float)player->currentPts / 1.0e9f;   // stream pts count from the start of the file
}

uint64_t getVideoPositionNs(const VideoPlayer *player) { return player->currentPts; }

float getVideoDurationSeconds(const VideoPlayer *player)
{
   return (float)player->demuxer.durationNs / 1.0e9f;
}

long getVideoLiveFirstSq(const VideoPlayer *player)
{
   if (!player || !player->demuxer.isMp4) return -1;
   const VideoSource *source = &player->demuxer.container.mp4.source;
   return source->isOpen && source->isLive ? getLiveSourceFirstSq(source->live) : -1;
}

const char *getVideoDecoderName(const VideoPlayer *player)
{
   if (player && player->vp9) return "TEE Decoder";   // TEE: the format is on the video line, the decoder is the TEE Decoder
   return player ? getH264DecoderName(player->decoder) : "none";
}

int isVideoOnTeeDecoder(const VideoPlayer *player)
{
   return player && (player->vp9 || isTeeH264Decoder(player->decoder));
}

int getVideoFrameLayout(VideoPlayer *player, VideoFrameLayout *out)
{
   if (!player || !out) return 0;
   lock(&player->lock);
   int idx = player->displayIndex;
   if (idx < 0) { unlock(&player->lock); return 0; }
   const FrameSlot *slot = &player->slots[idx];
   if (slot->hasVp9) {
      // the decoder's frame buffer: planes as wide as the stream rounded up to 64, a border on every side (the
      // module's 80 luma / 48 chroma samples, read back from the stride), each plane starting 128-byte aligned
      const tvp9_picture *pic = &slot->vp9;
      int aw = (pic->width + 63) & ~63, ah = (pic->height + 63) & ~63;
      int borderY = (pic->stride_y - aw) / 2, borderC = (pic->stride_c - aw / 2) / 2;
      out->y = pic->y - (size_t)borderY * pic->stride_y - borderY;
      out->u = pic->u - (size_t)borderC * pic->stride_c - borderC;
      out->v = pic->v - (size_t)borderC * pic->stride_c - borderC;
      out->pitchY = pic->stride_y;          out->pitchC = pic->stride_c;
      out->planeW = pic->stride_y;          out->planeH = ah + 2 * borderY;
      out->planeWC = pic->stride_c;         out->planeHC = ah / 2 + 2 * borderC;
      out->left = borderY;                  out->top = borderY;
      out->width = pic->width;              out->height = pic->height;
      out->leftC = borderC;                 out->topC = borderC;
   } else {
      int w = slot->width, h = slot->height;
      out->y = slot->yuv;                   out->u = slot->yuv + w * h;      out->v = out->u + w * h / 4;
      out->pitchY = w;                      out->pitchC = w / 2;
      out->planeW = w;                      out->planeH = h;
      out->planeWC = w / 2;                 out->planeHC = h / 2;
      out->left = out->top = 0;             out->width = w;                  out->height = h;
      out->leftC = out->topC = 0;
   }
   unlock(&player->lock);
   return out->y != 0;
}

// audio track format for a stats overlay; rate/channels read 0 when there's no audio track.
void getAudioTrackInfo(const VideoPlayer *player, int *rate, int *channels)
{
   const VideoDemuxer *source = player->hasAudioDemuxer ? &player->audioDemuxer : &player->demuxer;
   if (rate)     *rate = source->audioRate;
   if (channels) *channels = source->audioChannels;
}

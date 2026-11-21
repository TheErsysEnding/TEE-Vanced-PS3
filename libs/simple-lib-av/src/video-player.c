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
#include "tee_vp9/rsx_mc.h"    // (V3.1.0 Test 4) its RSX part: rsx_mc_drop_prealloc, rsx_mc_test_inject
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
   // (V3.1.0 Test 3) the flip of the last frame that drew it (getVideoFrameLayout): RETIRING goes once that flip happened
   uint32_t drawnFlip;
   int      drawn;
   uint64_t retiringUs; // when it left the screen (a flip that never comes holds it no longer than RETIRE_FENCE_MAX_US)
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
#define VP9_LOOKAHEAD_NS   10000000000ull   // (Test 14) the rule looks this far ahead in the 1440p stream's cues
#define VP9_STAY_NS        10000000000ull   // ... and goes back up to 1440p no sooner than this after a change
#define VP9_DOWN_BPS_SPE   15000000.0       // T_down / T_up on the SPEs alone - the VP9 session's estimate from five
#define VP9_UP_BPS_SPE     12000000.0       //   clips, logged with every change so they can be tuned
#define VP9_DOWN_BPS_RSX   30000000.0       // ... and with the RSX part predicting (its rsx_frames grow)
#define VP9_UP_BPS_RSX     25000000.0
#define VP9_BEHIND_NS      150000000ll      // the safety net: the picture this far behind its sound ...
#define VP9_BEHIND_US      1000000ull       // ... for this long - or two holds of the sound within VP9_HOLDS_US
#define VP9_HOLDS_US       6000000ull
#define VP9_DROPS_PER_S    8                // (V3.0.4) ... or this many pictures a second dropped as late or missing
#define VP9_DROP_SECONDS   2                //   for this many seconds in a row (one late a second is 60 fps on 59.94 Hz)
#define VP9_SETTLE_NS      6000000000ull    // ... though not this soon after a change of stream: its first seconds drop
                                            //   pictures of their own while the decoder takes the new size
#define VP9_DROP_IGNORE    2                // ... nor in the first seconds after a seek
#define AV_HOLD_CATCHUP_NS 1500000000ll     // (V3.0.4) while 1080p is on its way: the sound waits only this far behind
#define VP9_CATCHUP_US     20000000ull      // ... for this long at most
// (V3.0.4 Test 2) TEE VP9 stops on a key frame whose records - the entropy stage's output, about 7 times the block -
// outgrow its 5 MB buffer: LHGughAkidk's at 5.5 s (933 KB, records 6.8 MB) and 55 s (748 KB, 5.3 MB) in 2560x1350.
// A 1440p key frame larger than this goes in 1080p (vp9ConsiderChange), from the 1080p key frame at the same time.
// For scale: the largest of LXb3EKWsInQ's are 499 KB, of the Elden Ring DLC's (tqE50cUXGCs) 748, 662 and 640 KB.
#define VP9_KEY_FRAME_BYTES (600u << 10)
#define VP9_KEY_FRAME_MIN   (256u << 10)    // ... a stop on a smaller one lowers the limit, to no less than this
#define VP9_KEYS_KEPT       8               // ... the last 1440p key frames fed: the one a stop came on
#define VP9_RESTARTS        3               // ... a player opens TEE VP9 anew after a stop at most this often
#define VP9_SAME_TIME_NS    8000000ull      // (Test 3) ... two key frames this close are at the same time (half a frame)

// (V3.1.0) 4K - YouTube's 2160p VP9 at 24 / 25 / 30 fps (itag 313) - on TEE VP9 0.3.2, the SPEs alone, with the
// video's 1440p VP9 stream beside it for the busy stretches the way 1080p is beside 1440p. The VP9 project measured
// (0.3.1, SPEs alone, 7 pictures, 4 tasks): a London rain walk at 18 Mbit/s 33.8 pictures a second, Costa Rica at 25
// Mbit/s 31.3, a forest in wind at 22 Mbit/s 25.5, fireworks at 44 Mbit/s 14.6 - an ordinary 4K30 video holds, a
// heavy one does not. A 60 fps video has no 4K30 stream (its 4K is 60 fps: 315), so it never comes here.
#define VP9_4K_ROWS         1472            // a stream taller (or wider than 2560) is 4K: the decoder is opened for its size
#define VP9_DOWN_BPS_4K     20000000.0      // T_down / T_up for 4K on the SPEs alone - an estimate from those passages,
#define VP9_UP_BPS_4K       16000000.0      //   logged with every change so they can be tuned
#define VP9_KEY_FRAME_BYTES_4K (1200u << 10)   // a 4K key frame above this goes in 1440p: 0.3.x gives each of the two
                                            //   entropy SPEs 5 MB of a 4K frame's records (about 7 times the block)
#define VP9_SLOTS_4K        3               // the ring at 4K: one ready picture beside the two on screen - every buffer
                                            //   is 13.5 MB, and each one the ring holds the decoder has not
// (V3.1.0) TEE VP9 0.3.2 leaves out the shown frames no other frame refers to when they would come too late:
// tvp9_skip_before(clock + this), the VP9 project's measured margin (+ 0 ms leaves almost nothing out in time - the
// decision falls a few frames ahead -, + 200 ms leaves out more without being better)
#define VP9_SKIP_LEAD_NS    100000000ll

// (Test 2) what the decoder is opened with, the first that fits: picture buffers (its default 10 + 2 for the ring
// above, 6.5 MB each at 1440p) and how much input it may hold before it says "full". YouTube's 1440p60 averages
// about 10 Mbit/s but runs at 27 - 37 Mbit/s for half a minute in a busy scene (Elden Ring DLC at 17:11, Test 1):
// more than the PS3 downloads. The input is the cushion that rides such a stretch out: 20 MB is its queue of
// 256 frames (about 4 s) at the peak rate, the module's default 6 MB a little over 1 s of it.
// (V3.1.0 Test 4) and the sizes of its pools of records in MB (0: the decoder's own)
typedef struct { int pictures, tasks; uint32_t inputMb, entropyPoolMb, recordsPoolMb; } Vp9Setup;   // tasks 0: the decoder's own (5)
static const Vp9Setup VP9_SETUPS[] = { { 12, 0, 20, 0, 0 }, { 10, 0, 20, 0, 0 }, { 10, 0, 6, 0, 0 } };
// (V3.1.0) 4K, with the decoder's tasks (frames in its pixel stage, their records in its pool): 8 pictures of 13.5 MB
// and 10 MB of input are 159 MB - about what 1440p's first setup takes (154 MB; 18 MB stayed free beside it on TEE's
// console, Test 3) -, then the VP9 project's measured ones (7 pictures and 4 tasks 142 MB, 6 and 3 126 MB with 6 MB)
static const Vp9Setup VP9_SETUPS_4K[] = { { 8, 4, 10, 0, 0 }, { 7, 4, 10, 0, 0 }, { 7, 4, 6, 0, 0 }, { 6, 3, 6, 0, 0 } };
// (V3.1.0 Test 4) with the RSX part (vp9-rsx=): it takes 20 - 29 MB beside the decoder's own (prediction surfaces in
// tvp9_open; at the first key frame a 3 MB vertex ring and 1 MB of rows a slot), so less input and a picture less. 1440p
// (MB = 2^20, from tee_vp9.c's and rsx_mc.c's sizes): 11 pictures and 8 MB of input 158 MB in all, 10 and 8 MB 152, 10 and
// 6 MB 150 - the VP9 project's measured one; the first setup above would need 177. 4K: 7 pictures, 3 tasks, 3 MB of input and
// pools of 6 / 12 MB 156 MB (not measured), then the VP9 project's recommendation, 6 pictures and 3 tasks (152 MB)
static const Vp9Setup VP9_SETUPS_RSX[] = { { 11, 0, 8, 0, 0 }, { 10, 0, 8, 0, 0 }, { 10, 0, 6, 0, 0 } };
static const Vp9Setup VP9_SETUPS_4K_RSX[] = { { 7, 3, 3, 6, 12 }, { 6, 3, 3, 0, 0 } };

static int vp9Spes = 6;           // see setVideoVp9Spes
// (V3.1.0 Test 3) the display's counters (setVideoDisplayHooks) and the switches of display-sync= and vp9-release=
static VideoDisplayHooks displayHooks;
static int haveDisplayHooks;
static int syncByFlips = 1;
static int releaseEarly = 1;
#define RETIRE_FENCE_MAX_US 250000ull   // a flip that has not happened by then (the flip handler stopped?) holds no picture
// (V3.1.0 Test 4) TEE VP9's RSX part (setVideoVp9Rsx). Its state in the block is file-static (rsx_mc.c), so at most one
// decoder has it at a time, and a player left behind turns it off for the run
static int vp9RsxMode;                     // 0 off, 1 up to 1440p, 2 4K too
static const char *vp9RsxTest = "";        // settings.txt rsx-test=
static volatile int vp9RsxSessionOff;      // off for the rest of the run (disableVp9RsxForSession)
static volatile int vp9RsxLive;            // decoders open with rsx = 1
static int vp9RsxRingTaken;                // an RSX part started once: the block keeps its 3 MB vertex ring from here on
#define VP9_RSX_TRUST_PICTURES 30          // pictures an rsx = 1 decoder has to get its RSX part going in, else built anew
#define VP9_RSX_FREE_MB        8           // what has to stay free once the RSX part has what it takes at the first key frame
#define VP9_RSX_PAUSE_US       2000000     // the screen's pause and the RSX's drain around tvp9_close
#define VP9_RSX_STALL_ROWS     1500        // rsx-test=stall-3000: the RSX stands still 3 s after this many rows (about 1 s)
static uint32_t vp9KeyFrameLimit = VP9_KEY_FRAME_BYTES;   // (V3.0.4 Test 2) lowered for the rest of the run by a stop
static uint32_t vp9KeyFrameLimit4k = VP9_KEY_FRAME_BYTES_4K;   // (V3.1.0) ... the same for 4K's key frames

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
   // (V3.0.0 Test 14) the 1080p VP9 stream beside the 1440p one (createVideoPlayerVp9): the feeder changes over to it
   // ahead of a stretch the decoder cannot keep at 60 in 1440p, by the bitrate the 1440p stream's cues give, and back
   VideoDemuxer     lowDemuxer;
   int              hasLowDemuxer;
   volatile int     vp9OnLow;         // the feeder reads lowDemuxer (1080p), not demuxer (1440p)
   int64_t          vp9SwitchAtNs;    // feeder: a change waits for the other stream's key frame at this time (-1 none)
   int              vp9SwitchToLow;   // ... and goes this way
   const char      *vp9SwitchWhy;     // ... for the log
   int              vp9SwitchBps;     // ... the busiest section ahead when it was decided (bits a second)
   uint64_t         vp9NextCueNs;     // feeder: the next 1440p cue at which the rule is looked at again
   uint64_t         vp9LastSwitchNs;  // media time of the last change
   double           vp9DownBps, vp9UpBps;   // T_down / T_up on the SPEs alone, bits a second (the safety net lowers them)
   int              vp9Lowered;       // the safety net lowered them: they hold with the RSX part too
   volatile int     vp9SafetyRequest; // UI -> feeder: 1440p fell behind - down at the next cue
   volatile int     vp9SafetyBps;     // ... the bitrate of the section it fell behind in
   uint64_t         vp9BehindSinceUs; // UI: since when the picture has been more than VP9_BEHIND_NS behind (0 = it is not)
   uint64_t         vp9LastHoldUs;    // UI: when the sound's last hold began (two close together: the safety net)
   int              vp9DropSeconds;   // (V3.0.4) UI: seconds in a row in which VP9_DROPS_PER_S pictures were dropped or missing
   int              vp9DropIgnore;    // ... stats seconds still to pass over (a seek's)
   int              vp9CatchingUp;    // (V3.0.4) UI: 1080p is asked for - the sound runs on until the picture caught up
   uint64_t         vp9CatchingUpUs;  // ... since when
   struct { uint64_t pts; int size; } vp9Keys[VP9_KEYS_KEPT];   // ... feeder: the last 1440p key frames fed
   int              vp9KeyCount;      // ... how many (the ring's write count, 0 again at a seek)
   int64_t          vp9LastPts;       // ... decode thread: the last picture's time (-1: none since the open or a seek)
   tvp9_config      vp9Config;        // ... what TEE VP9 was opened with: restartVp9OnLow opens it the same way
   int              vp9Restarts;      // ... how often it was opened anew
   int              vp9RsxOn;         // the decoder's rsx_frames grew: the RSX thresholds apply
   int              vp9RsxFrames;     // ... its value at the last stats line
   int              vp9FramesAtStats; // (V3.1.0 Test 4) ... and the frames' (rsx_frames stood still while they grew: given up)
   int              vp9Rsx;           // ... TEE VP9 is open with its RSX part (rsx = 1)
   int              vp9MemoryAppMapped;   // ... vp9Memory came from the UI's allocator (freeFrame); rsx = 1: memalign, free()
   volatile int     vp9RsxTrusted;    // ... its RSX part predicts and the block has mapped the pictures: they are drawn
   int              vp9RsxUntrusted;  // ... pictures before that (VP9_RSX_TRUST_PICTURES: the player fails, built anew)
   int              vp9RsxShown;      // ... pictures since (rsx-test=brake-after-30)
   int              vp9RsxMemLogged;  // ... the memory line after its first picture
   volatile int     failedOnRsx;      // ... the player failed because its RSX part did not start (didVideoPlayerFailOnRsx)
   int              vp9Is4k;          // (V3.1.0) the main stream is 4K: its thresholds, key-frame limit and ring
   int              vp9Slots;         // ... the slots the ring may fill (VP9_SLOTS, VP9_SLOTS_4K)
   const char      *vp9HighName, *vp9LowName;   // ... "1440p" and "1080p", or "2160p" and "1440p" (the log's words)
   uint64_t     vp9StatsUs;       // when the decoder's numbers last went to the log
   uint64_t     vp9CountersUs;    // (V3.1.0 Test 3) ... and into totals
   uint64_t     vp9FirstPts;      // the rate measurement for a WebM without a frame duration
   int          vp9FirstIndex;    // ... and its picture number: (V3.1.0) a frame left out has a number too
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
   volatile int     failed;       // (V3.0.4) ... or the decoder stopped with an error (hasVideoPlayerFailed)

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
   int      statsMissed;        // (V3.1.0 Test 3) presents with none ready at which a picture was due (statsStarved counts
                                //   those at which none was due too: half of them at 30 fps on a 60 Hz screen)

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
   uint32_t syncLastMissed;     // (V3.1.0 Test 3) the display's missed-vblank count at the last present (display-sync=flips)
   uint64_t syncAtPts;          // ... where display sync's timeline stands: the picture on screen, or past it by the frames
                                //     TEE VP9 left out (their vblanks repeat the picture before them)
   VideoPlayerCounters totals;  // (V3.1.0 Test 3) what the player has done, for the bench (getVideoPlayerCounters)
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

// (V3.1.0 Test 3) rule 5 of TEE VP9's RSX part, for every picture: a slot that left the screen goes once the flip of the
// last frame that drew it has happened - the RSX has read it by then (it draws in the order of the command stream, and
// that flip comes after the draw). Until V3.1.0 Test 2 a slot left at the second present with a new picture: at 30 fps on
// a 60 Hz screen a picture 4K's ring of three needs back stayed a whole present longer than it had to (caller holds
// player->lock)
static int slotFlipDone(const FrameSlot *slot, uint64_t nowUs)
{
   if (!haveDisplayHooks || !slot->drawn) return 1;
   if ((int32_t)(displayHooks.flipsDone() - slot->drawnFlip) >= 0) return 1;
   return nowUs - slot->retiringUs > RETIRE_FENCE_MAX_US;
}

// at every present, not only at one with a new picture (vp9-release=early)
static void retireFlippedSlots(VideoPlayer *player, uint64_t nowUs)
{
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_RETIRING && slotFlipDone(&player->slots[i], nowUs)) retireSlot(player, i);
}

// (V3.1.0 Test 4) a picture of a decoder with the RSX part goes back only after the flip of its last draw, whatever
// vp9-release says: with the RSX shared there is no wait for the flip in the command stream, and the RSX may still read the
// picture as a texture until that flip (rule 5)
static int releaseByFlip(const VideoPlayer *player)
{
   return releaseEarly || player->vp9Rsx;
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
      if (inUse >= player->vp9Slots) { unlock(&player->lock); return -1; }   // (V3.1.0) 4K's ring is shorter
   }
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_EMPTY) { player->slots[i].state = SLOT_FILLING; player->slots[i].drawn = 0; found = i; break; }
   unlock(&player->lock);
   return found;
}

// VP9: feeds the demuxer's blocks while the decoder takes them (it copies each one at once and says "full" -
// 0 - when it has enough). Returns how many it fed, -1 on a read error. At the end of the input it tells the
// decoder and sets inputEnded.
// ---- (Test 14) the change between the 1440p and the 1080p VP9 stream ----
//
// Measured by the VP9 session at the Elden Ring passage (17:01 - 17:37): the decoder gives 53 - 57 pictures a second
// there in 1440p (the pixel SPEs), 86 - 97 in 1080p. Below 60 a player can only let the picture fall behind, drop
// pictures or hold the sound - so the player changes to 1080p before such a stretch, and back after it. Every WebM
// stream of YouTube's has cues: each section's start time and cluster offset, so the bitrate of every section is
// known without loading it.

// the cue at or before t (-1: before the first)
static int vp9CueAtOrBefore(const MkvDemuxer *mkv, uint64_t t)
{
   int lo = 0, hi = mkv->cueCount - 1, found = -1;
   while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (mkv->cues[mid].timeNs <= t) { found = mid; lo = mid + 1; } else hi = mid - 1;
   }
   return found;
}

// bits a second of section i: from cue i to the next one (the last one to the end of the stream)
static double vp9SectionBps(const MkvDemuxer *mkv, int i)
{
   if (i < 0 || i >= mkv->cueCount) return 0.0;
   uint64_t t0 = mkv->cues[i].timeNs, p0 = mkv->cues[i].clusterPos, t1, p1;
   if (i + 1 < mkv->cueCount) { t1 = mkv->cues[i + 1].timeNs; p1 = mkv->cues[i + 1].clusterPos; }
   else                       { t1 = mkv->durationNs;          p1 = mkv->segmentEnd; }
   if (t1 <= t0 || p1 <= p0) return 0.0;
   return (double)(p1 - p0) * 8.0e9 / (double)(t1 - t0);
}

// the busiest section of the 1440p stream from `from` (its own section too) to VP9_LOOKAHEAD_NS after it
static double vp9PeakAhead(const MkvDemuxer *mkv, uint64_t from)
{
   double peak = 0.0;
   int i = vp9CueAtOrBefore(mkv, from);
   if (i < 0) i = 0;
   for (; i < mkv->cueCount && mkv->cues[i].timeNs < from + VP9_LOOKAHEAD_NS; i++) {
      double bps = vp9SectionBps(mkv, i);
      if (bps > peak) peak = bps;
   }
   return peak;
}

// (V3.1.0) the key-frame limit of the player's main stream: 1440p's or 4K's (each lowered by a stop on its own)
static uint32_t *vp9KeyLimit(const VideoPlayer *player)
{
   return player->vp9Is4k ? &vp9KeyFrameLimit4k : &vp9KeyFrameLimit;
}

// (V3.1.0 Test 4) the RSX's thresholds are 1440p's: 4K keeps those of the SPEs alone until it is measured with the RSX
static int vp9RsxThresholds(const VideoPlayer *player)
{
   return player->vp9RsxOn && !player->vp9Lowered && !player->vp9Is4k;
}

static void vp9Thresholds(const VideoPlayer *player, double *down, double *up)
{
   if (vp9RsxThresholds(player)) { *down = VP9_DOWN_BPS_RSX; *up = VP9_UP_BPS_RSX; }
   else                          { *down = player->vp9DownBps; *up = player->vp9UpBps; }
}

// Mbit/s with one decimal, as two ints for the log (its printf has no floating point)
#define VP9_MBIT(bps) (int)((double)(bps) / 1000000.0), (int)((double)(bps) / 100000.0) % 10

// the feeder, for each block it read: is a change due here? The rule is looked at once a 1440p section (a cue,
// every ~3 s): down to 1080p when a section of the next 10 s is busier than T_down - or the safety net asked - and up
// again when every one is below T_up and the last change is 10 s back. A change waits for the other stream's key frame
// at or after that cue; once a block reaches it, the block is dropped, the other stream is set to its key frame there
// and the feeding goes on from it: TEE VP9 0.2.2 lets the frames before it finish and takes the new size, no flush,
// no picture lost. Returns 1 when the block was dropped.
static int vp9ConsiderChange(VideoPlayer *player, const VideoAu *au)
{
   if (!player->hasLowDemuxer) return 0;
   const MkvDemuxer *high = &player->demuxer.container.mkv;
   uint64_t pts = au->pts;
   // (V3.0.4 Test 2) a 1440p key frame too big for TEE VP9 (vp9KeyFrameLimit) never reaches it: 1080p from its key
   // frame at the same time, as seamless as a change for the bitrate - the stop it would have been is not a change
   // the player can make without a break (Test 1 built it anew in 1080p60 H.264 for the rest of the video; TEE: "warum
   // nicht so hybrid wie wir es bisher haben?"). The rule below takes it back up when the next 10 s allow - unless the
   // key frame there is too big as well, which sends it straight back here. Before a change planned for later, too.
   // (Test 3) Test 2 looked for that 1080p key frame among the cues only, and dropped 1440p up to the next one when
   // the key frame was not cued: YouTube cues every ~5.5 s but starts a cluster - on a key frame - every 0.03 - 7 s.
   // On LHGughAkidk 1440p's 681 KB key frame at 17.2 s starts such a cluster: 1440p dropped up to 1080p's cue at 22 s,
   // the picture held 4.8 s (TEE: "gefreezed auf 0 fps"), and again after a seek back. Now the change goes to 1080p's
   // cluster at that time, cued or not, and at once: each of the 1440p key frames above 500 KB of LHGughAkidk (9),
   // LXb3EKWsInQ (0) and the Elden Ring DLC (9 in its first 25 minutes) has a 1080p key frame at the same time.
   // (V3.1.0) 4K's key frames by 4K's limit, 1440p from its key frame at the same time
   uint32_t keyLimit = *vp9KeyLimit(player);
   if (!player->vp9OnLow && au->keyframe && (uint32_t)au->size > keyLimit) {
      uint64_t from = pts > VP9_SAME_TIME_NS ? pts - VP9_SAME_TIME_NS : 0;
      uint64_t lowAt = seekMkvDemuxerToCluster(&player->lowDemuxer.container.mkv, from);
      if (lowAt != MKV_NO_CLUSTER) {
         player->vp9OnLow = 1;
         player->totals.toLow++;   // (V3.1.0 Test 3)
         player->vp9LastSwitchNs = lowAt;
         player->vp9SwitchAtNs = -1;          // a change planned for later is this one
         player->vp9StartedAtKeyframe = 0;    // the cluster's first block is a key frame - if not, on to the next one
         if (lowAt <= pts + VP9_SAME_TIME_NS)
            logInfo("[video-player] VP9 quality: %s -> %s at %d.%03d s (a key frame too big for TEE VP9 in %s: "
                    "%d KB, the limit %d KB)\n", player->vp9HighName, player->vp9LowName, (int)(pts / 1000000000ull),
                    (int)(pts / 1000000ull % 1000), player->vp9HighName, au->size >> 10, (int)(keyLimit >> 10));
         else   // the two streams' key frames apart (none measured so): no picture for the time between
            logWarn("[video-player] VP9 quality: %s -> %s at %d.%03d s, %d ms after a key frame too big for TEE "
                    "VP9 in %s (%d KB, the limit %d KB): %s has none at its time - the picture holds meanwhile\n",
                    player->vp9HighName, player->vp9LowName, (int)(lowAt / 1000000000ull), (int)(lowAt / 1000000ull % 1000),
                    (int)((lowAt - pts) / 1000000ull), player->vp9HighName, au->size >> 10, (int)(keyLimit >> 10),
                    player->vp9LowName);
         return 1;   // this block dropped: the lower stream from its cluster
      }
      // no 1080p cluster header could be read: 1440p goes on - should TEE VP9 stop on it, restartVp9OnLow takes over
   }
   if (pts >= player->vp9NextCueNs) {   // a new 1440p section: the rule
      int i = vp9CueAtOrBefore(high, pts);
      uint64_t at = i >= 0 ? high->cues[i].timeNs : 0;
      player->vp9NextCueNs = i + 1 < high->cueCount ? high->cues[i + 1].timeNs : ~0ull;
      double down, up;
      vp9Thresholds(player, &down, &up);
      int safety = player->vp9SafetyRequest;
      if (safety) {   // T_down for the rest of the video: 90 % of the section 1440p fell behind in
         player->vp9SafetyRequest = 0;
         double lowered = 0.9 * (double)player->vp9SafetyBps;
         if (lowered > 2000000.0 && lowered < down) {
            player->vp9DownBps = lowered;
            if (player->vp9UpBps >= lowered) player->vp9UpBps = 0.8 * lowered;
            player->vp9Lowered = 1;
            vp9Thresholds(player, &down, &up);
         }
      }
      double peak = vp9PeakAhead(high, at);
      int wantLow = player->vp9OnLow;
      const char *why = NULL;
      if (!player->vp9OnLow && (safety || peak > down)) {
         wantLow = 1;
         why = safety ? "safety net: the picture fell behind" : "cues: a busy section ahead";
      } else if (player->vp9OnLow && peak < up && at >= player->vp9LastSwitchNs + VP9_STAY_NS) {
         wantLow = 0;
         why = "cues: the next 10 s are light";
      }
      if (why && player->vp9SwitchAtNs < 0) {
         const MkvDemuxer *target = wantLow ? &player->lowDemuxer.container.mkv : high;
         int k = vp9CueAtOrBefore(target, at);
         if (k < 0) k = 0;
         else if (target->cues[k].timeNs < at) k++;   // the target's key frame at or after this cue
         if (k < target->cueCount) {
            player->vp9SwitchAtNs = (int64_t)target->cues[k].timeNs;
            player->vp9SwitchToLow = wantLow;
            player->vp9SwitchWhy = why;
            player->vp9SwitchBps = (int)peak;
         }
      }
   }
   if (player->vp9SwitchAtNs < 0 || pts < (uint64_t)player->vp9SwitchAtNs) return 0;

   // the change: the other stream from its key frame at vp9SwitchAtNs
   uint64_t at = (uint64_t)player->vp9SwitchAtNs;
   int toLow = player->vp9SwitchToLow;
   player->vp9SwitchAtNs = -1;
   uint64_t landed = seekVideoDemuxer(toLow ? &player->lowDemuxer : &player->demuxer, at, 0);
   player->vp9OnLow = toLow;
   if (toLow) player->totals.toLow++; else player->totals.toHigh++;   // (V3.1.0 Test 3)
   player->vp9LastSwitchNs = landed;
   double down, up;
   vp9Thresholds(player, &down, &up);
   logInfo("[video-player] VP9 quality: %s -> %s at %d.%03d s (%s, %d.%d Mbit/s; T_down %d.%d, T_up %d.%d Mbit/s%s)\n",
           toLow ? player->vp9HighName : player->vp9LowName, toLow ? player->vp9LowName : player->vp9HighName,
           (int)(landed / 1000000000ull), (int)(landed / 1000000ull % 1000),
           player->vp9SwitchWhy, VP9_MBIT(player->vp9SwitchBps), VP9_MBIT(down), VP9_MBIT(up),
           vp9RsxThresholds(player) ? ", the RSX's" : player->vp9Lowered ? ", lowered" : "");
   return 1;
}

// a seek (decode thread, the feeder parked): straight onto the stream the rule wants at the target - the same
// thresholds as at a cue, the 10 s stay aside - so a seek into a busy stretch does not start it in 1440p
static uint64_t vp9SeekTrack(VideoPlayer *player, uint64_t targetNs, int landAfter)
{
   const MkvDemuxer *high = &player->demuxer.container.mkv;
   double down, up;
   vp9Thresholds(player, &down, &up);
   double peak = vp9PeakAhead(high, targetNs);
   int toLow = player->vp9OnLow ? !(peak < up) : peak > down;
   player->vp9SwitchAtNs = -1;
   player->vp9NextCueNs = 0;   // the rule looks again at the first block
   uint64_t landed = seekVideoDemuxer(toLow ? &player->lowDemuxer : &player->demuxer, targetNs, landAfter);
   if (toLow != player->vp9OnLow) {
      logInfo("[video-player] VP9 quality: %s -> %s at the seek to %d s (%d.%d Mbit/s ahead)\n",
              toLow ? player->vp9HighName : player->vp9LowName, toLow ? player->vp9LowName : player->vp9HighName,
              (int)(landed / 1000000000ull), VP9_MBIT(peak));
      player->vp9OnLow = toLow;
      player->vp9LastSwitchNs = landed;
   }
   return landed;
}

static int feedVp9(VideoPlayer *player, int *startedAtKeyframe)
{
   int fed = 0;
   while (fed < 8 && player->running && player->seekRequestNs < 0 && !player->inputEnded) {
      if (!player->hasPendingAu) {
         uint64_t readStartUs = sys_time_get_system_time();
         int r = readVideoAu(player->vp9OnLow ? &player->lowDemuxer : &player->demuxer, &player->pendingAu);   // (Test 14)
         uint64_t readUs = sys_time_get_system_time() - readStartUs;
         if (readUs > 30000) logWarn("[video-player] reading the next VP9 block took %d ms\n", (int)(readUs / 1000));
         if (r < 0) return -1;
         if (r == 0) { player->inputEnded = 1; tvp9_end_of_stream(player->vp9); break; }
         if (!*startedAtKeyframe) { if (!player->pendingAu.keyframe) continue; *startedAtKeyframe = 1; }
         if (vp9ConsiderChange(player, &player->pendingAu)) continue;   // (Test 14) dropped: the other stream from here
         player->hasPendingAu = 1;
      }
      int result = tvp9_feed(player->vp9, player->pendingAu.data, (size_t)player->pendingAu.size, (int64_t)player->pendingAu.pts);
      if (result == 0) break;            // the decoder has enough: retry this block later
      if (result < 0) {
         // (V3.0.4) a decoder that stopped refuses every block: 12870 lines of it in one log (TEE, 2026-11-14). Fed no
         // more until a seek starts it over (it clears vp9FeedError).
         if (tvp9_error(player->vp9)) {
            logWarn("[video-player] TEE VP9 stopped: no more blocks fed until a seek\n");
            player->hasPendingAu = 0;
            player->vp9FeedError = 1;
            return fed;
         }
         logWarn("[video-player] the VP9 decoder refused a block of %d bytes\n", player->pendingAu.size);
      }
      if (result > 0 && player->pendingAu.keyframe && !player->vp9OnLow) {   // (V3.0.4 Test 2) for restartVp9OnLow
         int at = player->vp9KeyCount++ % VP9_KEYS_KEPT;
         player->vp9Keys[at].pts = player->pendingAu.pts;
         player->vp9Keys[at].size = player->pendingAu.size;
      }
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
      // (V3.0.4 Test 2) no decoder: it did not open again after a stop (restartVp9OnLow) - the app builds the player anew
      if (player->inputEnded || player->vp9FeedError || player->seekRequestNs >= 0 || !player->vp9) { sleepMs(2); continue; }
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

// (V3.1.0) tells TEE VP9 0.3.2 where the clock stands, so it leaves out the frames no other one refers to that would
// come too late (tvp9_skip_before). Decode thread only: restartVp9OnLow closes and opens the decoder on it, so the
// handle is never one being closed. Nothing is left out before the picture runs (a pre-roll, a seek's) or while paused.
static void vp9SkipLate(VideoPlayer *player)
{
   if (!player->vp9) return;
   if (!player->started || player->paused || player->seekRequestNs >= 0) { tvp9_skip_before(player->vp9, INT64_MIN); return; }
   uint64_t clock = presentationClockNs(player, sys_time_get_system_time());
   tvp9_skip_before(player->vp9, (int64_t)clock + VP9_SKIP_LEAD_NS);
}

// VP9: the next picture into *pic (decode thread; the feeder fills the decoder meanwhile). 1 with a picture, 0 at the
// end of the stream, -1 on an error. Waiting on a slow network is no error as long as blocks keep coming in.
static int decodeNextVp9(VideoPlayer *player, tvp9_picture *pic)
{
   int stalledMs = 0;
   uint32_t fedSeen = player->vp9Fed;
   while (stalledMs < VP9_STALL_MS && player->running && player->seekRequestNs < 0) {
      vp9SkipLate(player);   // (V3.1.0) the clock as it moves, while it waits too
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
      // (V3.1.0 Test 3) a picture the screen let go of goes back while the decoder waits - it may be the buffer the next
      // picture needs (until Test 2 only claimEmptySlot handed them back, after the next picture had come out)
      if (releaseByFlip(player)) {
         int any = 0;
         for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].state == SLOT_RELEASE) any = 1;
         if (any) { lock(&player->lock); releaseVp9Slots(player); unlock(&player->lock); }
      }
      sleepMs(1); stalledMs++;
   }
   if (player->running && player->seekRequestNs < 0 && stalledMs >= VP9_STALL_MS) {
      if (player->inputEnded) return 0;
      logError("[video-player] TEE VP9: no picture and no input for %d ms\n", VP9_STALL_MS);
      return -1;
   }
   return 0;
}

// (V3.1.0 Test 4) TEE VP9's RSX part off for the rest of the run: later players open the decoder without it
static void disableVp9RsxForSession(const char *why)
{
   if (vp9RsxSessionOff) return;
   vp9RsxSessionOff = 1;
   logWarn("[video-player] TEE VP9's RSX part is off for the rest of this run: %s\n", why);
}

// ... a decoder with it opened anew (or closed): none of its pictures drawn, nor its thresholds used, until the latch trusts it
static void vp9RsxLatchReset(VideoPlayer *player)
{
   lock(&player->lock);
   player->vp9RsxTrusted = 0;
   player->vp9RsxOn = 0;
   unlock(&player->lock);
   player->vp9RsxUntrusted = 0;
   player->vp9RsxShown = 0;
   player->vp9RsxMemLogged = 0;
}

// ... after tvp9_close of a decoder with it: one less, and the crash marker goes if it was set
static void vp9RsxClosed(VideoPlayer *player)
{
   (void)__sync_fetch_and_sub(&vp9RsxLive, 1);
   if (player->vp9RsxTrusted && displayHooks.rsxActive) displayHooks.rsxActive(0);
   vp9RsxLatchReset(player);
}

// ... the trust latch, after each picture until it trusts the decoder (decode thread, no lock held). Its pictures are drawn
// only once the RSX part predicts (rsx_frames > 0 counts only frames that got an RSX slot) and the block has mapped the
// picture block for the RSX: the first key frame's set-up maps it (rsx_mc_init), and a set-up run again would unmap it under
// a picture being drawn - a started RSX part never runs it again. 1: go on; 0: VP9_RSX_TRUST_PICTURES pictures and it did
// not start - the player fails, and play.c builds it anew without the RSX part
static int vp9RsxTrust(VideoPlayer *player)
{
   tvp9_stats stats;
   tvp9_get_stats(player->vp9, &stats);
   if (!player->vp9RsxMemLogged) {
      sys_memory_info_t memory;
      player->vp9RsxMemLogged = 1;
      if (sys_memory_get_user_memory_size(&memory) == CELL_OK)
         logInfo("[video-player] user memory at TEE VP9's first picture with rsx = 1: %u MB free of %u MB (heap not counted)\n",
                 (unsigned)(memory.available_user_memory >> 20), (unsigned)(memory.total_user_memory >> 20));
   }
   int mapped = displayHooks.isMapped && displayHooks.isMapped(player->vp9Memory);
   if (stats.rsx_frames > 0 && mapped) {
      lock(&player->lock);
      player->vp9RsxTrusted = 1;
      player->vp9RsxOn = 1;
      unlock(&player->lock);
      vp9RsxRingTaken = 1;
      if (displayHooks.rsxActive) displayHooks.rsxActive(1);   // the crash marker: a run that ends now, the next goes without
      logInfo("[video-player] TEE VP9: RSX part predicts (rsx_frames %d after %d pictures) - its pictures are drawn from here on\n",
              stats.rsx_frames, player->vp9RsxUntrusted + 1);
      return 1;
   }
   if (++player->vp9RsxUntrusted < VP9_RSX_TRUST_PICTURES) return 1;
   logWarn("[video-player] TEE VP9: %d pictures and its RSX part did not predict (rsx_frames %d, the pictures %smapped) - the "
           "player is built anew without it\n", player->vp9RsxUntrusted, stats.rsx_frames, mapped ? "" : "not ");
   disableVp9RsxForSession("its RSX part did not start");
   return 0;
}

// VP9, after each picture (decode thread): the decoder's numbers to the log every 10 s, and - for a WebM whose
// track has no frame duration - the frame rate from the first pictures' time stamps, so display sync can lock on
static void afterVp9Picture(VideoPlayer *player, const tvp9_picture *pic)
{
   uint64_t nowUs = sys_time_get_system_time();
   // (V3.1.0 Test 3) the decoder's numbers for the bench, about once a second (word by word: a torn read costs nothing)
   if (nowUs - player->vp9CountersUs >= 1000000ull) {
      player->vp9CountersUs = nowUs;
      tvp9_stats now;
      tvp9_get_stats(player->vp9, &now);
      player->totals.vp9Frames = now.frames;
      player->totals.vp9Pictures = now.pictures;
      player->totals.vp9Skipped = now.skipped;
      player->totals.vp9RsxFrames = now.rsx_frames;
      player->totals.vp9EntropyWaits = now.entropy_waits;
      player->totals.vp9RecordsWaits = now.records_waits;
      player->totals.vp9BusyFpsX10 = (int)(now.fps * 10.0);
   }
   if (!player->vp9StatsUs) player->vp9StatsUs = nowUs;
   else if (nowUs - player->vp9StatsUs >= 10000000ull) {
      player->vp9StatsUs = nowUs;
      tvp9_stats stats;
      tvp9_get_stats(player->vp9, &stats);
      // integers only: this printf has no floating point
      if (stats.rsx_frames > player->vp9RsxFrames) player->vp9RsxOn = 1;   // (Test 14) the RSX part predicts
      // (V3.1.0 Test 4) ... and no more: frames grew and rsx_frames did not - the block gave the RSX up (or found it no faster)
      else if (player->vp9RsxOn && stats.frames > player->vp9FramesAtStats) player->vp9RsxOn = 0;
      player->vp9RsxFrames = stats.rsx_frames;
      player->vp9FramesAtStats = stats.frames;
      // (V3.1.0) the frames 0.3.2 left out, and those that waited for room in its pools of records (0.3.1)
      logInfo("[video-player] TEE VP9: %d frames, %d pictures, %d.%d pictures/s while busy, entropy %d us, pixel %d us a frame, "
              "%s %s, rsx_frames %d, %d left out, %d + %d waited for room\n", stats.frames, stats.pictures, (int)stats.fps,
              (int)(stats.fps * 10.0) % 10, (int)(stats.entropy_ms * 1000.0), (int)(stats.pixel_ms * 1000.0),
              !player->hasLowDemuxer ? "one stream" : "on", !player->hasLowDemuxer ? "" : player->vp9OnLow ?
              player->vp9LowName : player->vp9HighName, stats.rsx_frames, stats.skipped, stats.entropy_waits,
              stats.records_waits);
   }
   if (player->demuxer.frameDurationNs) return;
   if (player->vp9RatePictures == 0) { player->vp9FirstPts = (uint64_t)pic->pts; player->vp9FirstIndex = pic->index; }
   if (++player->vp9RatePictures <= VP9_RATE_PICTURES) return;
   uint64_t span = (uint64_t)pic->pts > player->vp9FirstPts ? (uint64_t)pic->pts - player->vp9FirstPts : 0;
   int frames = pic->index - player->vp9FirstIndex;   // (V3.1.0) the frames between them, left-out ones too
   player->vp9RatePictures = 0;
   if (!span || frames <= 0) return;
   lock(&player->lock);
   player->demuxer.frameDurationNs = span / (uint64_t)frames;
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
// slices per picture only shows while decoding), or no picture for the stall window. It is flushed and goes on
// from the next keyframe; when the flush fails, a new TEE Decoder takes over from there. Returns 1 when decoding
// can go on. The old decoder is closed before the next one opens, so their SPEs never add up.
// (V3.0.3, asked by TEE) A second failure within 30 s, or a failed flush, used to hand the stream to cellVdec for
// the rest of the video (Test 15). cellVdec plays 1080p60 at about 35 fps: a stall of a few seconds - "no SPE
// answered", seen on a slow connection, the input dry - became a video that lagged to its end. The TEE Decoder
// now always comes back; cellVdec plays only what the TEE Decoder cannot open at all (a coded width that is not
// a multiple of 128 - portrait videos, Shorts - too little memory) and video-decoder=sony.
static int recoverDecoder(VideoPlayer *player, int *startedAtKeyframe)
{
   if (!player->decoder || !isTeeH264Decoder(player->decoder) || player->inputEnded) return 0;
   uint64_t now = sys_time_get_system_time();
   int again = player->decoderFailureUs && now - player->decoderFailureUs < 30000000ull;
   player->decoderFailureUs = now;
   player->hasPendingAu = 0;
   *startedAtKeyframe = 0;
   if (resetH264Decoder(player->decoder) == 0) {
      logWarn("[video-player] TEE Decoder flushed after a failure%s, going on from the next keyframe\n",
              again ? " (the second within 30 s)" : "");
      return 1;
   }
   destroyH264Decoder(player->decoder);
   player->decoder = createTeeH264DecoderFor(&player->streamParams);
   if (!player->decoder) { logError("[video-player] no decoder after the TEE Decoder failed\n"); return 0; }
   setH264CatchUpSkip(player->decoder, 0);   // fresh decoder, fresh start: re-armed while behind
   logWarn("[video-player] its flush failed: a new TEE Decoder goes on from the next keyframe\n");
   return 1;
}

// (V3.0.4 Test 2) TEE VP9 stopped in 1440p after all - on a frame whose records outgrew its 5 MB buffer although its
// block was within vp9KeyFrameLimit, or on another error of its own. A stopped decoder takes no flush: it is closed and
// opened anew on the same memory, and goes on in 1080p from the 1080p key frame after the last picture, the sound
// running on - no "Loading", no new player. The screen is black until the first new picture: the new decoder clears
// the buffers the last ones are in. A stop on a key frame lowers vp9KeyFrameLimit below it for the rest of the run.
// 1 done, 0 not possible (a stop in 1080p, no 1080p stream, opened anew too often, it did not open): the app takes
// over then (hasVideoPlayerFailed). (Test 3) That key frame is the first 1080p cluster's after the last picture, cued
// or not (seekMkvDemuxerToCluster) - Test 2 waited for the next cue, up to 5.5 s of black.
static int restartVp9OnLow(VideoPlayer *player)
{
   const char *why = tvp9_error(player->vp9);
   if (!why || !player->hasLowDemuxer || player->vp9Restarts >= VP9_RESTARTS) return 0;
   int64_t last = player->vp9LastPts;

   // the feeder parks, as for a seek: it reads the demuxer this moves, and nothing may be fed while the decoder closes
   player->vp9FeedParked = 0;
   __sync_synchronize();
   player->vp9FeedPause = 1;
   __sync_synchronize();
   while (player->vp9FeedActive && !player->vp9FeedParked && !player->vp9FeedExited && player->running) sleepMs(1);
   if (!player->running) return 0;

   // the key frame it stopped on, if it was one: the first 1440p key frame fed after the last picture, close behind it
   // (the frames in the pixel stage when it stopped do not come out)
   uint64_t frameNs = player->demuxer.frameDurationNs ? player->demuxer.frameDurationNs : 20000000ull;
   int keyBytes = 0;
   uint64_t keyPts = 0;
   for (int n = 0; n < VP9_KEYS_KEPT && n < player->vp9KeyCount; n++) {
      int i = (player->vp9KeyCount - 1 - n) % VP9_KEYS_KEPT;
      if ((int64_t)player->vp9Keys[i].pts > last && (int64_t)player->vp9Keys[i].pts <= last + (int64_t)(8 * frameNs)) {
         keyBytes = player->vp9Keys[i].size;
         keyPts = player->vp9Keys[i].pts;
      }
   }
   // in 1080p - the feeder on it, and no 1440p key frame right after the last picture - it would stop there again
   if (player->vp9OnLow && !keyBytes) { player->vp9FeedPause = 0; return 0; }
   // (Test 3) the sound runs on from here, as after a change (it waits only for a picture 1.5 s behind): finding the
   // 1080p key frame can take a moment - a cluster header over the network (RPCS3: 0.4 s, the sound held 300 ms)
   lock(&player->lock);
   player->vp9CatchingUp = 1;
   player->vp9CatchingUpUs = sys_time_get_system_time();
   unlock(&player->lock);
   // the 1080p key frame after the last picture: the next cue's when it is right there (a stop on a cued key frame:
   // nothing to fetch), else the first 1080p cluster's after the picture, cued or not
   const MkvDemuxer *low = &player->lowDemuxer.container.mkv;
   int k = 0;
   while (k < low->cueCount && (int64_t)low->cues[k].timeNs <= last) k++;
   uint64_t at = k < low->cueCount ? low->cues[k].timeNs : MKV_NO_CLUSTER;
   int walked = 0;
   if (at == MKV_NO_CLUSTER || (int64_t)at > last + (int64_t)(8 * frameNs)) {
      uint64_t cluster = seekMkvDemuxerToCluster(&player->lowDemuxer.container.mkv, (uint64_t)(last + 1));
      if (cluster != MKV_NO_CLUSTER) { at = cluster; walked = 1; }
   }
   if (at == MKV_NO_CLUSTER) {
      lock(&player->lock);
      player->vp9CatchingUp = 0;
      unlock(&player->lock);
      player->vp9FeedPause = 0;
      return 0;
   }
   player->vp9Restarts++;
   uint32_t *keyLimit = vp9KeyLimit(player);   // (V3.1.0) 4K's own
   if (keyBytes) {
      uint32_t lowered = (uint32_t)keyBytes / 8 * 7;
      if (lowered < VP9_KEY_FRAME_MIN) lowered = VP9_KEY_FRAME_MIN;
      if (lowered < *keyLimit) *keyLimit = lowered;
   }
   logWarn("[video-player] TEE VP9 stopped in %s (%s), on %s: opened anew, on in %s from %d.%03d s, the sound "
           "running on%s (key frames above %d KB go in %s)\n", player->vp9HighName, why,
           keyBytes ? "a key frame" : "a frame", player->vp9LowName, (int)(at / 1000000000ull),
           (int)(at / 1000000ull % 1000), player->vp9Restarts > 1 ? " - again" : "", (int)(*keyLimit >> 10),
           player->vp9LowName);
   if (keyBytes)
      logInfo("[video-player] the key frame at %d.%03d s: %d KB\n", (int)(keyPts / 1000000000ull),
              (int)(keyPts / 1000000ull % 1000), keyBytes >> 10);

   // every slot goes: the buffers their pictures are in belong to the new decoder. From the key frame right after the
   // last picture, 1080p catches up as after a change (the sound waits only for a picture 1.5 s behind); up to one
   // further on - a stop within a section - the picture counts as being there already: else the sound would wait for
   // a picture that is not due yet, and neither would come
   uint64_t nowUs = sys_time_get_system_time();
   lock(&player->lock);
   for (int i = 0; i < FRAME_SLOTS; i++) {
      player->slots[i].hasVp9 = 0;
      player->slots[i].state = SLOT_EMPTY;
   }
   player->displayIndex = -1;
   if ((int64_t)at > last + (int64_t)(8 * frameNs) && at > player->currentPts) player->currentPts = at - 1;
   player->vp9SafetyRequest = 0;
   player->vp9BehindSinceUs = player->vp9LastHoldUs = 0;
   player->vp9DropSeconds = 0;
   player->vp9CatchingUp = 1;
   player->vp9CatchingUpUs = nowUs;
   uint32_t lastFlip = haveDisplayHooks ? displayHooks.nextFlipId() : 0;   // (V3.1.0 Test 3) the frame being drawn
   unlock(&player->lock);
   // the frame in flight: the RSX may still draw the last picture. (V3.1.0 Test 3) Until that frame's flip has happened -
   // at most 1 s - rather than a guess of 40 ms
   if (haveDisplayHooks) {
      for (int waited = 0; waited < 1000 && player->running && (int32_t)(displayHooks.flipsDone() - lastFlip) < 0; waited++)
         sleepMs(1);
   } else {
      sleepMs(40);
   }
   // (V3.1.0 Test 4) with the RSX part, tvp9_close unmaps the picture block, the prediction surfaces and the vertex ring - and
   // an unmap wants the RSX idle (libgcm reference, cellGcmUnmapEaIoAddress): the screen pauses (the UI writes no commands)
   // and the RSX is drained first, and drawing goes on right after the close. The new decoder has the same configuration,
   // the RSX part again, its pictures drawn once the latch trusts it
   int rsx = player->vp9Rsx, paused = rsx && displayHooks.pause && displayHooks.resume;
   if (paused) displayHooks.pause(VP9_RSX_PAUSE_US);
   tvp9_close(player->vp9);
   if (paused) displayHooks.resume();
   if (rsx) vp9RsxClosed(player);
   char err[160];
   err[0] = 0;
   player->vp9 = tvp9_open(&player->vp9Config, err, sizeof err);
   if (!player->vp9) {   // the smaller input, as openVp9's last setup
      tvp9_config smaller = player->vp9Config;
      smaller.input_bytes = 6u << 20;
      player->vp9 = tvp9_open(&smaller, err, sizeof err);
   }
   if (!player->vp9) {
      logError("[video-player] TEE VP9 did not open again: %s\n", err);
      player->vp9FeedPause = 0;   // (it waits: no decoder)
      return 0;
   }
   if (rsx) {
      (void)__sync_fetch_and_add(&vp9RsxLive, 1);
      vp9RsxLatchReset(player);
   }
   uint64_t landed = walked ? at : seekVideoDemuxer(&player->lowDemuxer, at, 0);
   player->vp9OnLow = 1;
   player->vp9LastSwitchNs = landed;
   player->vp9SwitchAtNs = -1;
   player->vp9NextCueNs = 0;
   player->hasPendingAu = 0;
   player->inputEnded = 0;
   player->vp9FeedError = 0;
   player->vp9StartedAtKeyframe = 0;
   player->vp9RatePictures = 0;
   player->vp9StatsUs = 0;
   player->vp9RsxFrames = 0;
   player->vp9FramesAtStats = 0;   // (V3.1.0 Test 4)
   player->vp9LastPts = (int64_t)landed - 1;
   vp9SkipLate(player);   // (V3.1.0) the new decoder leaves out what is late from its first frame on
   __sync_synchronize();
   player->vp9FeedPause = 0;
   return 1;
}

// flushes both pipelines and jumps to the seek target (decode thread only). The frame on screen
// stays up until the new position's frames arrive.
static void performSeek(VideoPlayer *player)
{
   uint64_t targetNs = (uint64_t)player->seekRequestNs;
   // (V3.0.4 Test 2) TEE VP9 did not open again after a stop (restartVp9OnLow): nothing to seek with - the app builds
   // the player anew (hasVideoPlayerFailed)
   if (player->vp9Memory && !player->vp9) { player->seekRequestNs = -1; return; }

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
      player->vp9KeyCount = 0;   // (V3.0.4 Test 2)
   }
   // flush the decoder; if the reset wedged it (EndSeq fatal) or it already died, rebuild it rather
   // than going dark (a NULL decoder makes decode report end-of-stream until a later seek revives it)
   else if (!player->decoder || resetH264Decoder(player->decoder) != 0) {
      logWarn("[video-player] decoder reset failed, recreating decoder\n");
      // (V3.0.3) a TEE Decoder comes back as a TEE Decoder, never as cellVdec (see recoverDecoder)
      int wasTee = isTeeH264Decoder(player->decoder);
      destroyH264Decoder(player->decoder);
      player->decoder = wasTee ? createTeeH264DecoderFor(&player->streamParams) : createH264DecoderFor(&player->streamParams);
      setH264CatchUpSkip(player->decoder, 0);   // fresh decoder, fresh start: re-armed while behind
   }
   player->hasPendingAu = 0;
   player->inputEnded = 0;
   player->vp9FeedError = 0;   // (V3.0.0 Test 12) a seek is a fresh try at the stream
   player->failed = 0;         // (V3.0.4)
   // land on the fragment keyframe covering the target - or, for a sponsor skip, the next keyframe at/after it
   uint64_t landedNs = player->hasLowDemuxer ? vp9SeekTrack(player, targetNs, player->seekLandAfter)   // (Test 14)
                                             : seekVideoDemuxer(&player->demuxer, targetNs, player->seekLandAfter);
   player->vp9LastPts = (int64_t)landedNs - 1;   // (V3.0.4 Test 2) no picture since
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
   player->statsShown = player->statsLate = player->statsRepeats = player->statsStarved = player->statsMissed = 0;
   player->vp9DropSeconds = 0;           // (V3.0.4) and the safety net's count of seconds with pictures dropped,
   player->vp9DropIgnore = VP9_DROP_IGNORE;   // the pre-roll's own seconds passed over
   player->vp9CatchingUp = 0;            // the pre-roll below holds and releases the sound itself
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
      int rsxFailOver = 0;   // (V3.1.0 Test 4) the RSX part did not start: the player fails, play.c builds it anew without it
      if (player->vp9) {
         tvp9_picture *pic = &player->slots[slot].vp9;
         got = decodeNextVp9(player, pic);
         if (got == 1) {
            width = pic->width; height = pic->height; pts = (uint64_t)pic->pts;
            player->slots[slot].hasVp9 = 1;   // published with SLOT_READY under the lock below
            afterVp9Picture(player, pic);
            player->vp9LastPts = (int64_t)pts;   // (V3.0.4 Test 2)
            if (player->vp9Rsx && !player->vp9RsxTrusted && !vp9RsxTrust(player)) {   // (V3.1.0 Test 4)
               tvp9_release_picture(player->vp9, pic);
               player->slots[slot].hasVp9 = 0;
               got = -1;
               rsxFailOver = 1;
            } else if (player->vp9Rsx && player->vp9RsxTrusted && ++player->vp9RsxShown == 30 &&
                       strcmp(vp9RsxTest, "brake-after-30") == 0 && displayHooks.brake) {
               displayHooks.brake("TEST: rsx-test=brake-after-30");   // (the block's next row finds no room: it gives up)
            }
         }
      } else {
         got = player->decoder ? decodeNextFrame(player, player->slots[slot].yuv, &width, &height, &pts, &startedAtKeyframe) : -1;
      }
      // (V3.0.4 Test 2) TEE VP9 stopped in 1440p: opened anew, on in 1080p (it empties every slot, this one too)
      if (got < 0 && !rsxFailOver && player->vp9 && player->running && player->seekRequestNs < 0 && restartVp9OnLow(player)) continue;
      if (got < 0 && !rsxFailOver && player->running && player->seekRequestNs < 0 && recoverDecoder(player, &startedAtKeyframe)) {
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
         if (player->seekRequestNs < 0) {
            player->ended = 1;
            if (got < 0) player->failed = 1;   // (V3.0.4) an error, not the end: hasVideoPlayerFailed
            if (rsxFailOver) player->failedOnRsx = 1;   // (V3.1.0 Test 4) ... didVideoPlayerFailOnRsx
         }
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

// (Test 14) the 1080p VP9 stream, opened beside the others the same way
static void lowOpenThread(uint64_t arg)
{
   AudioOpenJob *job = (AudioOpenJob *)(uintptr_t)arg;
   job->ok = openVideoDemuxer(&job->player->lowDemuxer, job->path) == 0;
   exitThread();
}

// (Test 14) the 1080p stream is kept when it fits beside the 1440p one: both VP9 with cues, the same frame rate, not
// larger. The first time, the log says whether the two put their key frames at the same times (YouTube's usual)
static void checkVp9LowTrack(VideoPlayer *player)
{
   const VideoDemuxer *high = &player->demuxer, *low = &player->lowDemuxer;
   const char *why = !high->isVp9 || !low->isVp9 ? "not both VP9" :
                     high->container.mkv.cueCount < 2 || low->container.mkv.cueCount < 2 ? "a stream without cues" :
                     low->width > high->width || low->height > high->height ? "it is not smaller" :
                     high->frameDurationNs && low->frameDurationNs && high->frameDurationNs != low->frameDurationNs ?
                        "another frame rate" : NULL;
   if (why) {
      logWarn("[video-player] the %s VP9 stream is left out (%s): no change of quality\n", player->vp9LowName, why);
      closeVideoDemuxer(&player->lowDemuxer);
      player->hasLowDemuxer = 0;
      return;
   }
   const MkvDemuxer *h = &high->container.mkv, *l = &low->container.mkv;
   int same = 0, n = l->cueCount < 50 ? l->cueCount : 50;
   for (int i = 0; i < n; i++) {
      int k = vp9CueAtOrBefore(h, l->cues[i].timeNs);
      if (k >= 0 && h->cues[k].timeNs == l->cues[i].timeNs) same++;
   }
   player->vp9DownBps = player->vp9Is4k ? VP9_DOWN_BPS_4K : VP9_DOWN_BPS_SPE;   // (V3.1.0) 4K's own
   player->vp9UpBps = player->vp9Is4k ? VP9_UP_BPS_4K : VP9_UP_BPS_SPE;
   logInfo("[video-player] VP9 %dx%d beside %dx%d: %d and %d cues, %d of the first %d key frames at the same times\n",
           low->width, low->height, high->width, high->height, l->cueCount, h->cueCount, same, n);
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

// (V3.1.0 Test 3) see video-player.h
void setVideoDisplayHooks(const VideoDisplayHooks *hooks)
{
   if (hooks && hooks->nextFlipId && hooks->flipsDone && hooks->vblanksMissed) { displayHooks = *hooks; haveDisplayHooks = 1; }
   else haveDisplayHooks = 0;
}
void setVideoDisplaySync(int byFlips) { syncByFlips = byFlips ? 1 : 0; }
void setVideoVp9ReleaseEarly(int early) { releaseEarly = early ? 1 : 0; }
// (V3.1.0 Test 4) see video-player.h
void setVideoVp9Rsx(int mode) { vp9RsxMode = mode == 1 || mode == 2 ? mode : 0; }
void setVideoVp9RsxTest(const char *test) { vp9RsxTest = test ? test : ""; }
int didVideoPlayerFailOnRsx(const VideoPlayer *player) { return player && player->failedOnRsx; }

void getVideoPlayerCounters(VideoPlayer *player, VideoPlayerCounters *counters)
{
   if (!counters) return;
   memset(counters, 0, sizeof *counters);
   counters->memFreeMb = -1;
   if (!player) return;
   lock(&player->lock);
   *counters = player->totals;
   counters->width = player->demuxer.width;
   counters->height = player->demuxer.height;
   counters->fpsMilli = player->demuxer.frameDurationNs ? (int)(1000000000000ull / player->demuxer.frameDurationNs) : 0;
   counters->onLow = isVideoOnLowTrack(player);
   counters->restarts = (uint32_t)player->vp9Restarts;
   unlock(&player->lock);
}

int isVideoVp9(const VideoPlayer *player) { return player && player->vp9; }
int isAudioOpus(const VideoPlayer *player) { return player && player->opus; }
// (V3.0.4 Test 2) by the picture on screen: the feeder reads seconds ahead of it, so its stream (vp9OnLow) told R3's
// panel "1440p" while 1080p still played
int isVideoOnLowTrack(const VideoPlayer *player)
{
   if (!player || !player->hasLowDemuxer) return 0;
   int idx = player->displayIndex;
   if (idx >= 0 && player->slots[idx].hasVp9) return player->slots[idx].height < player->demuxer.height;
   return player->vp9OnLow;
}

static void logVp9Line(const char *line) { logInfo("[tee-vp9] %s\n", line); }

// the open line's word on the sound
static const char *vp9SoundWord(const VideoPlayer *player)
{
   return player->hasAudioDemuxer && player->audioDemuxer.isOpus ? ", Opus sound on the PPU"
          : vp9Spes == 6 ? ", no sound (AAC needs an SPE)" : ", AAC sound";
}

// (V3.1.0 Test 4) the picture block back: to the UI's allocator, which mapped it - or, rsx = 1 (the block mapped it and
// tvp9_close unmapped it), to the heap
static void freeVp9Memory(VideoPlayer *player)
{
   if (!player->vp9Memory) return;
   if (player->vp9MemoryAppMapped) player->freeFrame(player->vp9Memory);
   else free(player->vp9Memory);
   player->vp9Memory = 0;
}

// (V3.1.0 Test 4) TEE VP9 with its RSX part for this stream? vp9-rsx= asks for it, the display shares the RSX (not given up,
// no crash marker found), no other decoder has it, the run has not turned it off - and 4K only with vp9-rsx=on
static int vp9RsxWanted(const VideoPlayer *player)
{
   static int unsharedLogged;
   if (!vp9RsxMode || vp9RsxSessionOff || vp9RsxLive != 0 || (vp9RsxMode == 1 && player->vp9Is4k)) return 0;
   if (!haveDisplayHooks || !displayHooks.rsxAllowed || !displayHooks.isMapped || !displayHooks.pause || !displayHooks.resume ||
       !displayHooks.rsxAllowed()) {
      if (!unsharedLogged) {
         unsharedLogged = 1;
         logWarn("[video-player] vp9-rsx: the display does not share the RSX (gfx-share off, the RSX part given up, or the last "
                 "run ended while it predicted) - TEE VP9 on the SPEs alone\n");
      }
      return 0;
   }
   return 1;
}

// ... the gate after an open with it: what the RSX part takes at the first key frame - the 3 MB vertex ring (once a run; the
// block keeps it), 1 MB of rows a slot (3 up to 2560 x 1472, 2 above), about 1 MB of rectangle lists - has to leave
// VP9_RSX_FREE_MB free, or the SPU groups run short of the system's memory (0x80010004 in the VP9 project's tests)
static int vp9RsxMemoryLeft(const VideoPlayer *player, int *freeMb)
{
   sys_memory_info_t memory;
   *freeMb = -1;
   if (sys_memory_get_user_memory_size(&memory) != CELL_OK) return 0;
   int needMb = VP9_RSX_FREE_MB + (vp9RsxRingTaken ? 0 : 3) + (player->vp9Is4k ? 2 : 3) + 1;
   *freeMb = (int)(memory.available_user_memory >> 20);
   return *freeMb >= needMb;
}

// ... the open: the RSX setups, the picture block in memory of its own that the app never maps (the block maps it at the
// first key frame and unmaps it in tvp9_close), the gate. None fits: the prediction surfaces tvp9_open took (17 - 24 MB) go
// back for the setups without the RSX part. 1: open, *config what it was opened with
static int openVp9Rsx(VideoPlayer *player, tvp9_config *config, char *err, int errLen)
{
   const Vp9Setup *setups = player->vp9Is4k ? VP9_SETUPS_4K_RSX : VP9_SETUPS_RSX;
   unsigned setupCount = player->vp9Is4k ? sizeof VP9_SETUPS_4K_RSX / sizeof VP9_SETUPS_4K_RSX[0]
                                         : sizeof VP9_SETUPS_RSX / sizeof VP9_SETUPS_RSX[0];
   for (unsigned setup = 0; setup < setupCount; setup++) {
      tvp9_config c = *config;
      c.rsx = 1;
      c.pictures = setups[setup].pictures;
      c.tasks = setups[setup].tasks;
      c.input_bytes = setups[setup].inputMb << 20;
      c.entropy_pool = setups[setup].entropyPoolMb << 20;
      c.records_pool = setups[setup].recordsPoolMb << 20;
      size_t bytes = tvp9_picture_memory_size(&c);
      void *memory = memalign(1024 * 1024, bytes);
      if (!memory) {
         snprintf(err, (size_t)errLen, "no 1 MB aligned block of %d MB for %d pictures", (int)(bytes >> 20), c.pictures);
         continue;
      }
      c.picture_memory = memory;
      c.picture_memory_bytes = bytes;
      if (strcmp(vp9RsxTest, "stall-3000") == 0) rsx_mc_test_inject(0, VP9_RSX_STALL_ROWS, 3000);   // (its next run)
      tvp9 *decoder = tvp9_open(&c, err, errLen);
      if (!decoder) {
         logWarn("[video-player] TEE VP9 with the RSX part did not open with %d pictures, %d tasks and %d MB input: %s\n",
                 c.pictures, c.tasks ? c.tasks : 5, (int)(c.input_bytes >> 20), err);
         free(memory);
         continue;
      }
      int freeMb;
      if (!vp9RsxMemoryLeft(player, &freeMb)) {
         tvp9_close(decoder);   // (nothing of its RSX part is set up before the first key frame: no pause, nothing unmapped)
         free(memory);
         snprintf(err, (size_t)errLen, "%d MB free with %d pictures and %d MB input, short of what its RSX part takes", freeMb,
                  c.pictures, (int)(c.input_bytes >> 20));
         logWarn("[video-player] TEE VP9 with the RSX part: %s\n", err);
         continue;
      }
      player->vp9 = decoder;
      player->vp9Memory = memory;
      player->vp9MemoryAppMapped = 0;
      player->vp9Rsx = 1;
      (void)__sync_fetch_and_add(&vp9RsxLive, 1);
      vp9RsxLatchReset(player);
      *config = c;
      logInfo("[video-player] TEE VP9 %s: %dx%d stream, %d + %d SPEs, %d pictures, %d tasks, %d MB input (%d MB + %d MB)%s, "
              "with the RSX part (rsx = 1; %d MB free)\n", TEE_VP9_VERSION, player->demuxer.width, player->demuxer.height,
              c.entropy_spes, c.pixel_spes, c.pictures, c.tasks ? c.tasks : 5, (int)(c.input_bytes >> 20), (int)(bytes >> 20),
              (int)(tvp9_memory_size(&c) >> 20), vp9SoundWord(player), freeMb);
      return 1;
   }
   if (vp9RsxLive == 0) rsx_mc_drop_prealloc();   // (rsx_mc.c does it only while nothing of it is set up)
   logWarn("[video-player] TEE VP9 with the RSX part does not fit (%s) - without it\n", err);
   return 0;
}

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
   // buffers for the stream's size, not the module's 2560 x 1440 default: a 1080p stream needs less memory.
   // (V3.1.0) A 4K stream (TEE VP9 0.3.0: up to 4096 x 2304) gets buffers as large as it is - and its 1440p stream
   // beside it fits them
   const Vp9Setup *setups = VP9_SETUPS;
   unsigned setupCount = sizeof VP9_SETUPS / sizeof VP9_SETUPS[0];
   if (player->vp9Is4k && player->demuxer.width <= 4096 && player->demuxer.height <= 2304) {
      config.max_width  = player->demuxer.width;
      config.max_height = player->demuxer.height;
      setups = VP9_SETUPS_4K;
      setupCount = sizeof VP9_SETUPS_4K / sizeof VP9_SETUPS_4K[0];
   } else if (player->demuxer.width > 0 && player->demuxer.height > 0 &&
              player->demuxer.width <= config.max_width && player->demuxer.height <= config.max_height) {
      config.max_width  = player->demuxer.width;
      config.max_height = player->demuxer.height;
   }
   char err[160];
   err[0] = 0;
   // (V3.1.0 Test 4) with the RSX part first, when it may run - its own setups and memory; without it the tables above, and
   // the prediction surfaces an earlier stream with it left in the heap (17 - 24 MB: rsx_mc keeps them) go back first - the
   // rebuild after a fail-over needs them most
   if (vp9RsxWanted(player)) openVp9Rsx(player, &config, err, (int)sizeof err);
   else if (vp9RsxLive == 0) rsx_mc_drop_prealloc();   // (nothing to do when there are none; rsx_mc.c refuses while in use)
   int pictures = 0;
   for (unsigned setup = 0; setup < setupCount && !player->vp9; setup++) {
      pictures = setups[setup].pictures;
      config.pictures = pictures;
      config.tasks = setups[setup].tasks;
      config.input_bytes = setups[setup].inputMb << 20;
      config.entropy_pool = setups[setup].entropyPoolMb << 20;   // (V3.1.0 Test 4: 0 in these, the decoder's own as before)
      config.records_pool = setups[setup].recordsPoolMb << 20;
      size_t bytes = tvp9_picture_memory_size(&config);
      player->vp9Memory = allocFrame(bytes);
      player->vp9MemoryAppMapped = 1;   // (V3.1.0 Test 4) the UI's allocator's: freeFrame gives it back
      if (!player->vp9Memory || ((uintptr_t)player->vp9Memory & 0xFFFFF)) {
         snprintf(err, sizeof err, "no 1 MB aligned block of %d MB for %d pictures", (int)(bytes >> 20), pictures);
         freeVp9Memory(player);
         continue;
      }
      config.picture_memory = player->vp9Memory;
      config.picture_memory_bytes = bytes;
      player->vp9 = tvp9_open(&config, err, sizeof err);
      if (!player->vp9) {
         logWarn("[video-player] TEE VP9 did not open with %d pictures, %d tasks and %d MB input: %s\n", pictures,
                 config.tasks ? config.tasks : 5, (int)(config.input_bytes >> 20), err);
         freeVp9Memory(player);
      }
      else logInfo("[video-player] TEE VP9 %s: %dx%d stream, %d + %d SPEs, %d pictures, %d tasks, %d MB input (%d MB + %d MB)%s\n",
                   TEE_VP9_VERSION, player->demuxer.width, player->demuxer.height, config.entropy_spes, config.pixel_spes,
                   pictures, config.tasks ? config.tasks : 5, (int)(config.input_bytes >> 20), (int)(bytes >> 20),
                   (int)(tvp9_memory_size(&config) >> 20), vp9SoundWord(player));
   }
   if (player->vp9) player->vp9Config = config;   // (V3.0.4 Test 2) for restartVp9OnLow
   sys_memory_info_t memory;
   if (player->vp9 && sys_memory_get_user_memory_size(&memory) == CELL_OK) {
      logInfo("[video-player] user memory with TEE VP9 open: %u MB free of %u MB (heap not counted)\n",
              (unsigned)(memory.available_user_memory >> 20), (unsigned)(memory.total_user_memory >> 20));
      player->totals.memFreeMb = (int)(memory.available_user_memory >> 20);   // (V3.1.0 Test 3)
   }
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
   return createVideoPlayerVp9(videoPath, NULL, audioPath, allocFrame, freeFrame);
}

VideoPlayer *createVideoPlayerVp9(const char *videoPath, const char *lowVideoPath, const char *audioPath,
                                  VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame)
{
   VideoPlayer *player = (VideoPlayer *)calloc(1, sizeof *player);
   if (!player) return 0;
   player->displayIndex = -1;
   player->seekRequestNs = -1;
   player->audioSeekPendingNs = -1;
   player->vp9SwitchAtNs = -1;
   player->vp9LastPts = -1;   // (V3.0.4 Test 2)
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

   // (Test 14) the 1080p VP9 stream beside a 1440p one, opened in parallel as well
   AudioOpenJob lowJob = { player, lowVideoPath, 0 };
   sys_ppu_thread_t lowOpenTid;
   int lowOpening = lowVideoPath && lowVideoPath[0] &&
                    spawnJoinableThread(&lowOpenTid, lowOpenThread, (uint64_t)(uintptr_t)&lowJob,
                                        THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "vp9-low-open") == 0;

   int videoOk = openVideoDemuxer(&player->demuxer, videoPath) == 0;
   // (V3.1.0) 4K: 1440p beside it, its own thresholds and key-frame limit, a shorter ring (openVp9 sets the decoder up)
   player->vp9Is4k = videoOk && player->demuxer.isVp9 &&
                     (player->demuxer.height > VP9_4K_ROWS || player->demuxer.width > 2560);
   player->vp9Slots = player->vp9Is4k ? VP9_SLOTS_4K : VP9_SLOTS;
   player->vp9HighName = player->vp9Is4k ? "2160p" : "1440p";
   player->vp9LowName = player->vp9Is4k ? "1440p" : "1080p";

   // join the audio open before ANY exit: the thread writes into the player, so it must be done
   // (and its outcome adopted) before a failed video open can tear the player down
   if (lowOpening) { joinThread(lowOpenTid); player->hasLowDemuxer = lowJob.ok; }
   if (lowOpening && !lowJob.ok)
      logWarn("[video-player] the %s VP9 stream did not open: no change of quality\n", player->vp9LowName);
   if (audioOpening) { joinThread(audioOpenTid); player->hasAudioDemuxer = audioJob.ok; }
   else if (audioPath && audioPath[0]) player->hasAudioDemuxer = openAudioDemuxer(&player->audioDemuxer, audioPath) == 0;
   if (audioPath && audioPath[0] && !player->hasAudioDemuxer)
      logWarn("[video-player] separate audio stream failed to open, playing silent\n");

   if (!videoOk) goto fail;

   if (player->hasLowDemuxer) checkVp9LowTrack(player);   // (Test 14)
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
   if (player->hasLowDemuxer) cancelVideoDemuxer(&player->lowDemuxer);       // (Test 14)
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
         // (V3.1.0 Test 4) the RSX part's state is file-static in the block (rsx_mc.c): with a decoder left open no other one
         // may have it - and vp9RsxLive stays counted
         disableVp9RsxForSession("a player was left behind");
         releaseAudioPipeline(player);
         return;
      }
      joinThread(player->thread);
      if (player->vp9FeedActive) joinThread(player->vp9FeedTid);
   }
   releaseAudioPipeline(player);
   destroyH264Decoder(player->decoder);
   if (player->vp9) {
      // (V3.1.0 Test 4) with the RSX part: the RSX idle before tvp9_close unmaps its memory (see restartVp9OnLow) - on the UI
      // thread, where play.c destroys a player after finishGfx, that is a drain only
      int rsx = player->vp9Rsx, paused = rsx && displayHooks.pause && displayHooks.resume;
      if (paused) displayHooks.pause(VP9_RSX_PAUSE_US);
      tvp9_close(player->vp9);                                     // the SPEs stop (by force if one hangs)
      if (paused) displayHooks.resume();
      if (rsx) vp9RsxClosed(player);
   }
   freeVp9Memory(player);
   closeVideoDemuxer(&player->demuxer);
   if (player->hasAudioDemuxer) closeVideoDemuxer(&player->audioDemuxer);
   if (player->hasLowDemuxer) closeVideoDemuxer(&player->lowDemuxer);
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
   int elapsed;
   if (haveDisplayHooks && syncByFlips) {
      // (V3.1.0 Test 3) every present is one flip, one vblank on screen - plus the vblanks the screen showed the last frame
      // again for, which the flip handler counts from the flips' times. Counted by wall time, a present that ran late
      // (a log write, a subtitle cue being rendered) but whose frame still made its vblank - the UI runs a frame ahead -
      // counted 2 and the next one 0: a picture dropped and one repeated while the screen missed nothing (40 of the 75
      // stutter seconds of VP9 in TEE's log of V3.0.4 Test 3)
      uint32_t missedNow = displayHooks.vblanksMissed();
      int missed = (int)(int32_t)(missedNow - player->syncLastMissed);
      player->syncLastMissed = missedNow;
      elapsed = 1 + (missed > 0 ? missed : 0);
      if (elapsed > 6) elapsed = 6;
   } else {
      int64_t vblankUs = (int64_t)player->syncVblankUs;
      elapsed = (int)(((int64_t)(nowUs - player->syncLastUs) + vblankUs / 2) / vblankUs);
      if (elapsed < 0) elapsed = 0;
   }
   player->syncLastUs = nowUs;
   player->syncVblanks += elapsed;
   int advance = player->syncVblanks / player->syncPerFrame;
   player->syncVblanks -= advance * player->syncPerFrame;

   int64_t frameNs = (int64_t)player->demuxer.frameDurationNs;
   // (V3.1.0 Test 3) where the timeline stands - display-sync=wall keeps Test 2's: the picture on screen
   int byFlips = haveDisplayHooks && syncByFlips;
   uint64_t at = byFlips && player->syncAtPts > player->currentPts ? player->syncAtPts : player->currentPts;
   int64_t errorNs = (int64_t)at - (int64_t)clock;   // > 0: the picture is ahead of the sound
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
      // (V3.1.0 Test 3) the next picture 1.5 - 4.5 frames on: TEE VP9 left up to three frames out (tvp9_skip_before). This
      // step is a left-out frame's, so the picture on screen stays for it - a repeat in its place, rather than the next
      // picture a vblank early and a repeat after it. A wider gap (a change of stream that lands later) is shown as before
      int64_t gapNs = (int64_t)(player->slots[next].pts - at);
      if (byFlips && player->vp9 && frameNs > 0 && gapNs > frameNs * 3 / 2 && gapNs <= frameNs * 9 / 2) {
         at += (uint64_t)frameNs;
         continue;
      }
      best = next;
      after = at = player->slots[next].pts;
   }
   player->syncAtPts = at;
   return best;
}

const uint8_t *getVideoFrame(VideoPlayer *player, int *width, int *height)
{
   uint64_t nowUs = sys_time_get_system_time();
   int releaseAudio = 0;   // set when pre-roll fires, to un-hold the audio feed after the lock is dropped
   char prerollLine[160];  // (V3.0.0 Test 13) where picture and sound stand when the pre-roll ends
   prerollLine[0] = 0;
   lock(&player->lock);
   // (V3.1.0 Test 3) the pictures the screen is done with go back at every present - before the returns of a seek or a
   // pre-roll too, which can hold a picture back for seconds
   if (releaseByFlip(player)) retireFlippedSlots(player, nowUs);

   // a posted seek freezes presentation: frames decoded before the flush must not advance the
   // position; the displayed frame stays up until the new position's frames arrive
   if (player->seekRequestNs >= 0) return takeHeldFrame(player, width, height);

   // anchor the clock to the earliest ready frame, with a short pre-roll: hold the first present until a
   // few frames are buffered (PREROLL_FRAMES) and the audio clock is live. Starting on the very first
   // decoded frame began playback with drops (the clock outran the cold decoder) and a visible stutter
   // when the audio clock later took over from the wall clock with a backward step.
   if (!player->started) {
      uint64_t earliest = 0; int found = -1, readyCount = 0, onScreen = 0;
      for (int i = 0; i < FRAME_SLOTS; i++) {
         if (player->slots[i].state == SLOT_READY) { readyCount++; if (found < 0 || player->slots[i].pts < earliest) { earliest = player->slots[i].pts; found = i; } }
         if (player->slots[i].state == SLOT_DISPLAYING || player->slots[i].state == SLOT_RETIRING) onScreen++;
      }
      // (V3.1.0) VP9: as many as the ring has room for beside the pictures a seek leaves on screen - 4K's ring of three
      // holds one then, and waiting for two would hold the picture for good
      int prerollNeed = PREROLL_FRAMES;
      if (player->vp9) {
         prerollNeed = player->vp9Slots - onScreen < VP9_PREROLL_FRAMES ? player->vp9Slots - onScreen : VP9_PREROLL_FRAMES;
         if (prerollNeed < 1) prerollNeed = 1;
      }
      int decoderWarm  = readyCount >= prerollNeed || player->ended;   // a short clip may never fill the ring
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
      // (V3.1.0) display sync locks on afresh, from the picture the clock picks: still locked from before a seek, it
      // looked only for pictures later than the position - and the seek's first picture IS the position. With 1440p's
      // ring it was dropped as late; 4K's ring of three it filled for good (the picture stood, RPCS3, 21 Nov)
      player->syncActive = 0;
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
      if (synced && best >= 0) {
         player->syncActive = 1; player->syncLastUs = nowUs; player->syncVblanks = 0;
         player->syncAtPts = bestPts;   // (V3.1.0 Test 3)
         if (haveDisplayHooks) player->syncLastMissed = displayHooks.vblanksMissed();   // (V3.1.0 Test 3)
      }
   }
   if (!synced) player->syncActive = 0;

   int late = 0;
   if (best >= 0) {
      for (int i = 0; i < FRAME_SLOTS; i++) {
         if (player->slots[i].state == SLOT_RETIRING) {                                                  // GPU is done with it by now -
            if (!releaseByFlip(player) || slotFlipDone(&player->slots[i], nowUs)) retireSlot(player, i);   // (Test 3) once its flip happened
         } else if (player->slots[i].state == SLOT_DISPLAYING) {                                          // hold it for the frame in flight
            player->slots[i].state = SLOT_RETIRING;
            player->slots[i].retiringUs = nowUs;
         }
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
   char safetyLine[160];   // (Test 14) the safety net's word, logged after the lock
   safetyLine[0] = 0;
   if (player->audioHeld && (player->paused || !player->vp9 || !player->audioFeedOpen || !player->audioClockValid)) {
      player->audioHeld = 0;   // the user's pause owns the feed now; at the end of the sound, what is left plays out
      if (!player->paused) holdAction = 2;
   } else if (player->vp9 && player->audioFeedOpen && player->audioClockValid && player->started && !player->paused) {
      // > 0: the picture is behind the sound. (V3.1.0 Test 3) Display sync's timeline where it is past the picture on screen
      // by a frame or three TEE VP9 left out: those repeats are no picture behind
      uint64_t shownPts = synced && player->syncActive && haveDisplayHooks && syncByFlips && player->syncAtPts > player->currentPts
                          ? player->syncAtPts : player->currentPts;
      int64_t behindNs = (int64_t)clock - (int64_t)shownPts;
      int readyAfter = 0, readyDue = 0;
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY) { readyAfter++; if (player->slots[i].pts <= clock) readyDue++; }
      // (V3.1.0) caught up: at the clock - or as near as the decoded pictures go, every one of them still to come. A held
      // sound freezes the clock, and without display sync (24 / 25 fps on a 60 Hz screen) only a picture that is due is
      // shown: the picture stopped a few ms before the clock - or before a frame TEE VP9 left out - and the sound waited
      // for a picture that waited for the sound
      int caughtUp = behindNs <= 0 || (readyDue == 0 && readyAfter > 0);
      // (V3.0.4) While 1080p is on its way - from the safety net's word until the picture has caught up after the
      // change - the sound waits only for a picture more than AV_HOLD_CATCHUP_NS behind. Each wait of the sound is a
      // stutter, and the frames already fed to the decoder still come in 1440p: four in ten seconds in a busy 1440p60
      // scene (TEE, 2026-11-14, LXb3EKWsInQ at 0:23). The picture drops what it is late for and catches up in 1080p.
      if (player->vp9CatchingUp &&
          ((player->vp9OnLow && behindNs < AV_HOLD_ENGAGE_NS / 2) || nowUs - player->vp9CatchingUpUs > VP9_CATCHUP_US))
         player->vp9CatchingUp = 0;
      int64_t holdAtNs = player->vp9CatchingUp ? AV_HOLD_CATCHUP_NS : AV_HOLD_ENGAGE_NS;
      if (!player->audioHeld) {
         // (V3.1.0 Test 3) not while every decoded picture is still to come: a gap in the pictures (frames TEE VP9 left
         // out, a change of stream that lands later) is no picture behind - held then, the sound and the picture would
         // wait for each other, and the release below lets go of it at once (the same test, caughtUp)
         if (behindNs > holdAtNs && !caughtUp && !player->ended) {
            player->audioHeld = 1;
            player->audioHeldSinceUs = nowUs;
            holdAction = 1;
            player->totals.holds++;   // (V3.1.0 Test 3)
            snprintf(holdLine, sizeof holdLine, "[video-player] the sound waits for the picture: %d ms behind it, %d decoded\n",
                     (int)(behindNs / 1000000), readyAfter);
         }
      } else {
         uint64_t heldUs = nowUs - player->audioHeldSinceUs;
         // (V3.1.0) "both ready slots": the ring's ready ones - one at 4K
         int holdReady = player->vp9Slots - 2 < AV_HOLD_READY ? player->vp9Slots - 2 : AV_HOLD_READY;
         if (holdReady < 1) holdReady = 1;
         if (player->ended || (caughtUp && (readyAfter >= holdReady || heldUs >= AV_HOLD_MAX_US)) ||
             (player->vp9CatchingUp && behindNs < holdAtNs / 2)) {   // (V3.0.4) 1080p is on its way: the sound goes on
            player->audioHeld = 0;
            holdAction = 2;
            player->totals.holdMs += (uint32_t)(heldUs / 1000);   // (V3.1.0 Test 3)
            snprintf(holdLine, sizeof holdLine, "[video-player] the sound runs on after %d ms, %d pictures decoded ahead\n",
                     (int)(heldUs / 1000), readyAfter);
         }
      }
      // (Test 14) the safety net: 1440p falls behind although the cues let it play - more than VP9_BEHIND_NS for
      // VP9_BEHIND_US, or the sound held twice within VP9_HOLDS_US. Then 1080p from the next cue, and T_down for the
      // rest of the video at 90 % of the bitrate where it happened (the feeder does both)
      if (player->hasLowDemuxer && !player->vp9OnLow && !player->vp9SafetyRequest) {
         const char *sign = NULL;
         if (behindNs > VP9_BEHIND_NS) {
            if (!player->vp9BehindSinceUs) player->vp9BehindSinceUs = nowUs;
            else if (nowUs - player->vp9BehindSinceUs > VP9_BEHIND_US) sign = "the picture behind for a second";
         } else {
            player->vp9BehindSinceUs = 0;
         }
         if (holdAction == 1) {
            if (player->vp9LastHoldUs && nowUs - player->vp9LastHoldUs < VP9_HOLDS_US) sign = "the sound waited twice";
            player->vp9LastHoldUs = nowUs;
         }
         // (V3.0.4) the earliest sign, seconds before the sound has to wait: pictures dropped as late or missing,
         // VP9_DROP_SECONDS seconds in a row (counted with the stutter statistics below)
         if (player->vp9DropSeconds >= VP9_DROP_SECONDS) sign = "pictures dropped";
         if (sign) {
            const MkvDemuxer *high = &player->demuxer.container.mkv;
            int bps = (int)vp9SectionBps(high, vp9CueAtOrBefore(high, player->currentPts));
            player->vp9SafetyBps = bps;
            __sync_synchronize();
            player->vp9SafetyRequest = 1;
            player->totals.safetyNets++;   // (V3.1.0 Test 3)
            player->vp9BehindSinceUs = player->vp9LastHoldUs = 0;
            player->vp9DropSeconds = 0;
            player->vp9CatchingUp = 1;   // (V3.0.4) the sound runs on from here until 1080p has caught up
            player->vp9CatchingUpUs = nowUs;
            snprintf(safetyLine, sizeof safetyLine, "[video-player] VP9: %s falls behind at %d s (%d.%d Mbit/s there, %s): "
                     "%s from the next cue, the sound runs on meanwhile\n", player->vp9HighName,
                     (int)(player->currentPts / 1000000000ull), VP9_MBIT(bps), sign, player->vp9LowName);
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
      // (V3.1.0 Test 3) ... and of those, the ones at which the next picture was due: at 30 fps every other present on a
      // 60 Hz screen needs none, and 4K's ring of three is often empty at such a present - which Test 2 counted as a
      // picture missing. On TEE's console London 4K30 dropped to 1440p after 8 s although every picture came in time
      int64_t frameDueNs = (int64_t)player->demuxer.frameDurationNs;
      if (best < 0 && readyCount == 0 &&
          (frameDueNs <= 0 || (int64_t)player->currentPts + frameDueNs <= (int64_t)clock + frameDueNs / 4)) player->statsMissed++;
      player->statsLate += late;
      if (player->started) {   // (V3.1.0 Test 3) the same, since the player was built - for the bench
         player->totals.presents++;
         if (best >= 0) player->totals.shown++; else player->totals.repeats++;
         if (best < 0 && readyCount == 0) player->totals.starved++;
         player->totals.late += (uint32_t)late;
      }
      if (readyCount < player->statsReadyMin) player->statsReadyMin = readyCount;
      if (nowUs - player->statsSecondUs >= 1000000ull) {
         // (V3.0.4) the safety net's earliest sign: a second in which 1440p dropped or missed VP9_DROPS_PER_S pictures -
         // not in the seconds after a change of stream, when the decoder takes the new size, nor right after a seek
         if (player->vp9DropIgnore > 0) {
            player->vp9DropIgnore--;
            player->vp9DropSeconds = 0;
         } else if (player->vp9 && player->hasLowDemuxer && !player->vp9OnLow && !player->ended &&
                    player->currentPts >= player->vp9LastSwitchNs + VP9_SETTLE_NS &&
                    player->statsLate + player->statsMissed >= VP9_DROPS_PER_S) {
            player->vp9DropSeconds++;
         } else {
            player->vp9DropSeconds = 0;
         }
         if (player->statsLate || player->statsMissed)   // (V3.1.0 Test 3) a second in which a picture was missing or late
            snprintf(statsLine, sizeof statsLine, "[video-player] 1 s: %d pictures shown, %d dropped as late, %d presents "
                     "without a new one (%d with none ready), fewest ready %d of %d, %d due with none ready\n",
                     player->statsShown, player->statsLate, player->statsRepeats, player->statsStarved, player->statsReadyMin,
                     FRAME_SLOTS, player->statsMissed);
         player->statsSecondUs = nowUs;
         player->statsShown = player->statsLate = player->statsRepeats = player->statsStarved = player->statsMissed = 0;
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
   if (safetyLine[0]) logInfo("%s", safetyLine);
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

int hasVideoPlayerFailed(const VideoPlayer *player) { return player && player->failed; }

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
   // TEE: the format is on the video line, the decoder is the TEE Decoder - (V3.1.0 Test 4) the R3 panel's " RSX" while its RSX
   // part predicts
   if (player && player->vp9) return player->vp9RsxOn ? "TEE Decoder RSX" : "TEE Decoder";
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
   FrameSlot *slot = &player->slots[idx];
   // (V3.1.0 Test 4) a picture of a decoder with the RSX part is drawn only once the latch trusts it (vp9RsxTrust) - not
   // stamped either, so it goes back at once
   if (slot->hasVp9 && player->vp9Rsx && !player->vp9RsxTrusted) { unlock(&player->lock); return 0; }
   // (V3.1.0 Test 3) the frame being drawn shows this picture: it goes back once that frame's flip has happened
   if (haveDisplayHooks) { slot->drawnFlip = displayHooks.nextFlipId(); slot->drawn = 1; }
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

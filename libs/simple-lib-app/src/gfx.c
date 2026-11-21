// 2D graphics implementation using libgcm/RSX.
// Triple-buffered, uncapped. Shader-based batched quad renderer.
#include "gfx.h"
#include "colors.h"
#include "dbg.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <sys/timer.h>
#include <sys/sys_time.h>
#include <sys/synchronization.h>   // (V3.1.0 Test 4) the command buffer's lock, shared with TEE VP9's RSX part
#include <sys/ppu_thread.h>        // ... and which thread draws (a pause from another one)
#include <cell/gcm.h>
#include <cell/codec/pngdec.h>
#include <cell/sysmodule.h>
#include <sysutil/sysutil_sysparam.h>

#include "../../simple-lib-av/src/tee_vp9/tv9_rsx_host.h"   // (V3.1.0 Test 4) what TEE VP9's RSX part asks of this file

#define CTX gCellGcmCurrentContext

#define FRAME_COUNT       3
#define HOST_MEM_SIZE     (1 * 1024 * 1024)
#define CMD_BUFFER_SIZE   (0x10000)
#define BYTES_PER_PIXEL   4

// (V3.1.0 Test 4) gfx-share=on, the command buffer shared with TEE VP9's RSX part (see "one command buffer" below)
#define SHARED_CMD_BYTES     0xF0000         // in the same 1 MB host block: cellGcmInit wants the buffer at least 1 KB short of
                                             // it (the RSX reads 1 KB ahead), and 64 KB stay mapped behind it
#define SHARED_SLACK_BYTES   256             // end stays this far before the piece's real end: room for the restart's jump
#define SHARED_SMALL_BYTES   (48 * 1024)     // rsx-test=small-cb: a piece this long, so that it starts over every frame or two
#define BURST_BYTES          8192            // room made before every group of the app's commands (the largest is about 3 KB)
#define DRAIN_LABEL          255             // the app's only label: 0 - 63 are the system's, 200 - 212 TEE VP9's
#define UI_LOCK_US           3000000         // the UI waits this long for the lock at most - then the RSX part is given up
#define BLOCK_LOCK_SLICE_US  2000000         // the block waits in slices of this (it cannot take a failure)
#define APP_RESTART_US       1000000         // the app's restart of the piece waits this long for the RSX (the block's: 2 s)
#define FLIP_WAIT_US         100000          // the wait for the flip before the next one; then this frame goes without one
#define FLIP_STALL_US        2000000         // no flip for this long: the RSX part is given up
#define DRAIN_US             2000000         // finishGfx, the end of a prediction
#define DRAIN_EXIT_US        500000          // termGfx
#define PAUSE_MAX_US         5000000         // a pause nobody ends is ended after this
#define SHARED_STATS_US      10000000ull     // the "[gfx] shared:" line
#define LOCAL_POOL_BYTES     (2u << 20)      // RSX memory for the block (1.3 MB at 1440p, 1.9 MB at 4K)
#define LOCAL_BLOCKS         16              // ... in pieces (it takes 13)
#define LAPS_LOGGED          20              // the first restarts of the piece get a line each
#define RSX_MARKER           DBG_DIR "/rsx-active"

// max quads per frame (6 verts each). the batch costs 24 bytes per vertex in two
// buffers, so this is 4.5 MB of the console's ~250 MB of graphics memory - cheap
// enough that no screen in the tree should ever have to draw around it.
#define MAX_QUADS         16384
#define MAX_VERTS         (MAX_QUADS * 6)

// what each shape costs out of that budget. composite shapes reserve their whole
// cost before drawing any part, so a shape at the cap goes missing rather than
// coming out half drawn.
#define VERTS_PER_TRIANGLE     3
#define VERTS_PER_RECT         6
#define VERTS_PER_LINE         (VERTS_PER_TRIANGLE * 2)
#define VERTS_PER_STROKED_RECT (VERTS_PER_RECT * 4)

// RSX hard limit on texture/surface dimensions (mirrors image-loader.c's MAX_TEX_DIM)
#define MAX_TEX_DIM       4096

typedef struct {
   float x, y, z;
   uint32_t rgba;
   float u, v;
} GfxVertex;

// Apps always draw on a virtual 1920x1080 canvas regardless of the TV's real resolution.
// The clip-space conversion divides by the virtual size while the viewport is the real
// surface, so the GPU scales everything down (720p, 576p, 480p) for free. On a 1080p
// screen virtual == physical and nothing changes.
#define VIRTUAL_W 1920
#define VIRTUAL_H 1080

static int       initialized = 0;
static int       screenW = 0;        // display buffer dimensions (the flipped surface, real pixels)
static int       screenH = 0;
// Logical drawing space of the CURRENT render target: the virtual canvas for the display
// buffer, or an offscreen target's own w*h. All draw-call math (clip-space, clamping)
// divides by these, which is what maps virtual coordinates onto the real viewport.
static int       targetW = 0;
static int       targetH = 0;
// Real pixel dimensions of the currently bound surface (viewport/scissor use these).
static int       surfaceW = 0;
static int       surfaceH = 0;
static int       targetIsDisplay = 0;   // drawing to the screen buffer (vs an offscreen target)
static uint32_t  framePitch = 0;
static uint32_t  frameOffset[FRAME_COUNT];
static uint32_t  backBuffer = 0;

static uint8_t  *vramBase = 0;
static size_t    vramSize = 0;

// free-list allocator state. The list tiles the whole VRAM region contiguously
// in address order, so list-adjacent blocks are memory-adjacent and can be
// coalesced on free. Node metadata lives in main RAM (malloc), not VRAM.
typedef struct VramBlock {
   void   *addr;   // start address of this block
   size_t  size;   // span of this block in bytes (== memory it covers)
   int     isFree; // 1 if free, 0 if allocated
   struct VramBlock *next;
} VramBlock;

static VramBlock *blockListHead = NULL;
static size_t     vramAllocated = 0;  // sum of allocated block sizes

// double-buffered vertex batch (write one while GPU reads the other)
static GfxVertex *batchVerts[FRAME_COUNT];
static uint32_t   batchOffset[FRAME_COUNT];
static uint32_t   batchColOffset[FRAME_COUNT];
static uint32_t   batchUvOffset[FRAME_COUNT];
static int        batchVertCount = 0;
static int        batchFlushStart = 0;
static int        frameDroppedCalls = 0;     // draw calls the frame could not afford

// textures handed back during a frame, released once that frame has been shown
#define GFX_RETIRED_MAX 256
static uint32_t   retired[GFX_RETIRED_MAX];
static uint32_t   retiredFlip[GFX_RETIRED_MAX];   // (V3.1.0 Test 4, shared) the flip after which each one may go
static int        retiredCount = 0;

static void freeRetiredTextures(void);

// shader state
extern struct _CGprogram _binary_vpshader_vpo_start;
extern struct _CGprogram _binary_fpshader_fpo_start;
extern struct _CGprogram _binary_fpshader_yuv_fpo_start;

static CGprogram shaderVp;
static CGprogram shaderFp;
static void     *shaderVpUcode;
static void     *shaderFpUcode;
static uint32_t  shaderFpOffset;
static uint32_t  shaderPosIdx;
static uint32_t  shaderColIdx;
static uint32_t  shaderUvIdx;
static uint32_t  shaderTexUnit;

// yuv video shader: converts YUV 4:2:0 planar frames to RGB on the RSX (see fpshader-yuv.cg)
static CGprogram shaderFpYuv;
static uint32_t  shaderFpYuvOffset;
static uint32_t  shaderYuvTexUnit[3];   // texY, texU, texV
static CGparameter shaderYuvChromaXf;    // (TEE Vanced) where the chroma is sampled, see fpshader-yuv.cg
static float     yuvChromaXfSet[4];      // the value the ucode holds now
static int       yuvChromaXfValid;

// 1x1 white texture for color-only draws
static uint32_t whiteTexOffset = 0;

// current batch texture state
static uint32_t batchTexOffset = 0;
static int      batchTexW = 1;
static int      batchTexH = 1;
static int      batchTexPitch = 64;
static int      batchTexLinear = 0;

static void flushBatch(void);

void *allocateVram(size_t size, size_t alignment)
{
   if (size == 0) return NULL;
   if (alignment == 0) alignment = 1;

   // first call: one free block covering the whole region.
   if (blockListHead == NULL) {
      blockListHead = (VramBlock *)malloc(sizeof(VramBlock));
      if (!blockListHead) return NULL;
      blockListHead->addr = vramBase;
      blockListHead->size = vramSize;
      blockListHead->isFree = 1;
      blockListHead->next = NULL;
   }

   VramBlock *prev = NULL;
   for (VramBlock *block = blockListHead; block; prev = block, block = block->next) {
      if (!block->isFree) continue;

      size_t base    = (size_t)block->addr;
      size_t aligned = (base + alignment - 1) & ~(alignment - 1);
      size_t padding = aligned - base;
      if (padding + size > block->size) continue;  // doesn't fit

      // carve the alignment padding off the front as its own free block,
      // linked in just before this one (prev is the predecessor) so the list
      // stays address-ordered.
      if (padding > 0) {
         VramBlock *padBlock = (VramBlock *)malloc(sizeof(VramBlock));
         if (!padBlock) return NULL;
         padBlock->addr = block->addr;
         padBlock->size = padding;
         padBlock->isFree = 1;
         padBlock->next = block;
         if (prev) prev->next = padBlock;
         else      blockListHead = padBlock;
         block->addr  = (void *)aligned;
         block->size -= padding;
      }

      // carve any leftover off the back as its own free block.
      if (block->size > size) {
         VramBlock *tailBlock = (VramBlock *)malloc(sizeof(VramBlock));
         if (!tailBlock) return NULL;
         tailBlock->addr = (uint8_t *)block->addr + size;
         tailBlock->size = block->size - size;
         tailBlock->isFree = 1;
         tailBlock->next = block->next;
         block->next = tailBlock;
         block->size = size;
      }

      block->isFree = 0;
      vramAllocated += block->size;
      return block->addr;
   }

   logError("[gfx] allocateVram: out of VRAM (%zu bytes)\n", size);
   return NULL;
}

void freeVram(void *ptr)
{
   if (!ptr) return;

   VramBlock *prev = NULL;
   for (VramBlock *block = blockListHead; block; prev = block, block = block->next) {
      if (block->addr != ptr) continue;
      if (block->isFree) return;  // double-free: ignore

      block->isFree = 1;
      vramAllocated -= block->size;

      // merge with the following block if it is free (memory-adjacent).
      if (block->next && block->next->isFree) {
         VramBlock *next = block->next;
         block->size += next->size;
         block->next  = next->next;
         free(next);
      }

      // merge into the preceding block if it is free.
      if (prev && prev->isFree) {
         prev->size += block->size;
         prev->next  = block->next;
         free(block);
      }
      return;
   }
}

// how long the last endGfxFrame spent blocked before it could reuse a buffer. triple-buffered and
// non-blocking, so the CPU stalls here only when the display pipeline is full (the GPU has fallen a
// whole frame behind); in the steady state it is ~0.
static uint64_t lastFlipWaitUs;

// the display pipeline. flipsSubmitted counts flips the CPU has queued; flipsCompleted counts the
// ones the RSX has retired (bumped by the flip callback). their difference is how many are in flight.
// (V3.1.0 Test 3) both volatile: the player reads them from its own threads (getGfxNextFlipId / getGfxFlipsDone), and
// the flip handler times the flips for the bench - a gap over 25 ms is a vblank the screen showed the last frame again
static volatile uint32_t flipsSubmitted;
static volatile uint32_t flipsCompleted;
static volatile uint64_t lastFlipUs;
static volatile uint32_t flipGaps25, flipGaps42, flipMaxGapUs;
static volatile uint32_t vblanksMissed;
static uint32_t lastFlipVblank;    // the vblank count when the handler saw the last flip
static uint64_t lastFlipTimeUs;    // libgcm's time of the last flip (cellGcmGetLastFlipTime)
static uint32_t screenVblankUs;    // one vblank of the screen, in microseconds (initGfx)
static void onGfxFlip(const uint32_t head)
{
   (void)head;
   uint64_t now = sys_time_get_system_time();
   if (lastFlipUs) {
      uint64_t gap = now - lastFlipUs;
      if (gap > 25000) flipGaps25++;
      if (gap > 41700) flipGaps42++;
      if (gap > flipMaxGapUs) flipMaxGapUs = (uint32_t)(gap > 0xFFFFFFFFull ? 0xFFFFFFFFull : gap);
   }
   lastFlipUs = now;
   // the vblanks between this flip and the last - more than one apart, the screen showed the last frame again meanwhile.
   // libgcm stamps every flip with the time it happened (cellGcmGetLastFlipTime), so a handler that wakes late moves
   // nothing: the distance in whole vblanks of the screen. (The vblank count the handler reads would be one too many
   // whenever it ran after the next vblank, and the next distance one too few.) Where that time does not move on - 0,
   // or the same as the last one's -, the vblank count stands in.
   uint64_t flipTime = (uint64_t)cellGcmGetLastFlipTime();
   uint32_t vblank = (uint32_t)cellGcmGetVBlankCount();
   uint32_t apart = 0;
   if (flipTime && lastFlipTimeUs && flipTime > lastFlipTimeUs && screenVblankUs) {
      uint64_t vblanks = (flipTime - lastFlipTimeUs + screenVblankUs / 2) / screenVblankUs;
      apart = vblanks > 600 ? 600 : (uint32_t)vblanks;
   } else if (lastFlipVblank) {
      apart = vblank - lastFlipVblank;
   }
   if (apart > 1 && apart < 0x80000000u) vblanksMissed += apart > 600 ? 599 : apart - 1;
   if (flipTime > lastFlipTimeUs) lastFlipTimeUs = flipTime;
   lastFlipVblank = vblank ? vblank : 1;
   __sync_synchronize();
   flipsCompleted++;
}

uint32_t getGfxNextFlipId(void) { return flipsSubmitted + 1; }
uint32_t getGfxFlipsDone(void)  { return flipsCompleted; }
uint32_t getGfxVblankCount(void) { return (uint32_t)cellGcmGetVBlankCount(); }
uint32_t getGfxVblanksMissed(void) { return vblanksMissed; }

void getGfxFlipStats(GfxFlipStats *stats)
{
   if (!stats) return;
   stats->flips = flipsCompleted;
   stats->gaps25 = flipGaps25;
   stats->gaps42 = flipGaps42;
   stats->maxGapUs = flipMaxGapUs;
}

uint64_t getGfxFlipWaitUs(void)
{
   return lastFlipWaitUs;
}

// ============================================================================
// (V3.1.0 Test 4) one command buffer for the app and TEE VP9's RSX part
// ============================================================================
//
// gfx-share=on (settings.txt, off by default and experimental: a mistake here hangs the console until it is switched off by
// hand): TEE VP9 0.3.2 predicts on the RSX while the app draws the screen with it - one command buffer, one RSX state.
// tee_vp9/tv9_rsx_host.h has the five rules; this is how gfx.c keeps them (the VP9 project's reference/gfx.c and vshow.c ran
// them on the console with PSL1GHT):
//  1. one lock, cmdLock (an lv2 mutex, FIFO, not recursive). The app holds it for one group of commands at a time - a burst,
//     gfxBurstBegin .. gfxBurstEnd: lock, room, the target state if needed, the commands, the flush, unlock - and the block
//     for one of its rows (tv9h_lock). Nothing but command writing happens under it: no log line, no other lock;
//  2. one piece of 955 KB from offset 0x1000 instead of libgcm's 32 KB segments, whose switch hung the RSX in the VP9
//     project's tests. Room is made before every burst; when it is short, the RSX reads everything, a jump takes it back to
//     the beginning and the next commands start there (gfxCbRestart, for the app and the block). libgcm's callback is
//     replaced by one that writes nothing (gfxCbOverflow): it is never meant to run;
//  3. after a flip, a new target or the block's rows, the next burst first sets everything the app's draws depend on
//     (emitTargetState), and every draw invalidates the vertex cache (the RSX may read a batch while the next is written);
//  4. no cellGcmSetWaitFlip in the command stream - the block's rows behind it would wait for a vertical blank. The PPU waits
//     for the flip before it queues the next one (never two pending) and meanwhile draws into the third buffer;
//  5. (video-player.c) a picture goes back to the decoder once the flip after its draw has happened.
// With gfx-share=off none of it runs: every command added here is behind gfxShareActive, and the app writes V3.1.0 Test 3's
// command stream (64 KB with libgcm's segments, cellGcmSetWaitFlip, two flips in flight).

static int gfxShareWanted, gfxSmallCb, gfxNoContextTest;   // setGfxShareMode (before initGfx)
static int gfxShareActive;                    // the layout check passed: everything below is in use
static sys_mutex_t cmdLock;                   // rule 1 - created once and never destroyed (tv9h_lock cannot do without it)
static int cmdLockOk;
static sys_ppu_thread_t uiThread;             // initGfx's thread: the one that draws
static uint32_t *cbBegin, *cbEnd;             // the piece: from offset 0x1000 to SHARED_SLACK_BYTES before its end
static uint32_t cbBeginOffset;
static int cbParked;                          // a restart waits for the RSX: begin = current = end = cbBegin meanwhile
static volatile unsigned stateGen;            // tv9h_state_gen: + 1 with every burst of the app's
static volatile unsigned blockGen;            // tv9h_lock calls: the block may have changed the state since
static unsigned blockGenSeen;
static uint32_t targetOffset, targetPitch;    // the render target setSurface recorded (its size: surfaceW x surfaceH)
static int frameStateValid;                   // emitTargetState ran since the last flip or change of target
static int frameBroken;                       // a burst failed: the rest of the frame writes nothing and has no flip
static int parkRetried;                       // a parked piece was tried in this frame (once a frame)
static volatile int burstBroken;              // the burst under way cannot be written whole (the callback was reached, no target)
static uint32_t *burstStart;
static volatile int rsxBrake;                 // TEE VP9's RSX part given up for the run (tv9h_context NULL from here on)
static const char *volatile brakeWhy;
static volatile int brakeLogged;
static int rsxCrashOff;                       // the last run ended while the RSX predicted: no RSX part in this one
static volatile int pauseCount;               // pauseGfxDrawing from another thread: the UI writes nothing meanwhile
static volatile uint64_t pauseSinceUs;
static uint32_t drainSeq;                     // label 255's last value (written under the lock)
static uint32_t flipsConfirmed;               // (UI thread) the flips known to have happened, by the handler or the status
static uint64_t lastFlipTimeUs;               // ... the last one's time (cellGcmGetLastFlipTime)
static int flipStallLogged;
static uint32_t flipBurstBytes;               // a flip's commands, for one log line
static int flipBurstLogged;
static volatile int lockError;                // a lock call failed other than by its timeout: logged once
static int lockErrorLogged;
static uint64_t blockLockedAtUs;              // when the block took the lock (its hold time)

// the 10-s line's counts, written by the UI thread and (laps, lock times) the block's - a count lost now and then is harmless
static struct {
   uint64_t sinceUs, bytes;
   uint32_t flips, gapMaxUs, gaps25, gaps42, resyncs, skipped, bursts, burstMax, resends, appLaps, blockLaps;
   uint32_t precheckTimeouts, restartTimeouts, drainTimeouts, overflows, uiWaits, uiWaitMaxUs, blockHoldMaxUs;
} shared;
typedef struct { uint32_t get, put, bytes; int byBlock; } LapRecord;
static LapRecord lapRecords[LAPS_LOGGED];     // the first restarts of the piece: get and put at the jump, the lap's bytes
static volatile int lapsRecorded;
static int lapsLogged;

// RSX memory for the block (tv9h_local_alloc): a pool of its own with a lock of its own - the UI's allocator above has none,
// and the block allocates on the decoder's thread
static uint8_t *localPool;
static uint32_t localUsed;
static int localLive;
static void *localBlocks[LOCAL_BLOCKS];
static sys_lwmutex_t localLock;

static uint64_t gfxNowUs(void) { return sys_time_get_system_time(); }

static sys_ppu_thread_t gfxThread(void)
{
   sys_ppu_thread_t id = 0;
   sys_ppu_thread_get_id(&id);
   return id;
}

// the RSX part given up for the run. Never a log line here - it can be under the lock; gfxLogBrake writes it outside
static void gfxBrake(const char *why)
{
   if (rsxBrake) return;
   brakeWhy = why;
   __sync_synchronize();
   rsxBrake = 1;
}

static void gfxLogBrake(void)
{
   if (!rsxBrake || brakeLogged || __sync_lock_test_and_set(&brakeLogged, 1)) return;
   logWarn("[gfx] shared: TEE VP9's RSX part is off for the rest of this run (%s) - the RSX draws the screen alone\n",
           brakeWhy ? brakeWhy : "no reason given");
}

// the lock for a burst of the UI's: at most UI_LOCK_US, then the RSX part is given up (the block holds it for a row, "well
// under a millisecond as a rule")
static int lockForUi(void)
{
   int rc = sys_mutex_trylock(cmdLock);
   if (rc != CELL_OK) {
      uint64_t startUs = gfxNowUs();
      rc = sys_mutex_lock(cmdLock, UI_LOCK_US);
      uint32_t waitedUs = (uint32_t)(gfxNowUs() - startUs);
      shared.uiWaits++;
      if (waitedUs > shared.uiWaitMaxUs) shared.uiWaitMaxUs = waitedUs;
   }
   if (rc == CELL_OK) return 1;
   if (rc == ETIMEDOUT) gfxBrake("the UI waited 3 s for the command buffer's lock");
   else if (!lockError) lockError = rc;
   return 0;
}

// ... for a drain, from any thread
static int lockForDrain(uint32_t timeoutUs)
{
   int rc = sys_mutex_lock(cmdLock, timeoutUs ? timeoutUs : 1);   // (0 would wait for ever)
   if (rc == CELL_OK) return 1;
   if (rc != ETIMEDOUT && !lockError) lockError = rc;
   return 0;
}

static void unlockCmd(void)
{
   sys_mutex_unlock(cmdLock);
}

// Writes a linear (CELL_GCM_SURFACE_PITCH) RGBA color surface as the render target and sets the viewport + scissor to its
// real w*h - the commands setSurface wrote until V3.1.0 Test 3, and still does with gfx-share=off (shared, emitTargetState
// writes them)
static void writeSurfaceState(uint32_t offset, uint32_t pitch, int w, int h)
{
   CellGcmSurface sf;
   memset(&sf, 0, sizeof(sf));

   sf.colorFormat      = CELL_GCM_SURFACE_A8R8G8B8;
   sf.colorTarget      = CELL_GCM_SURFACE_TARGET_0;
   sf.colorLocation[0] = CELL_GCM_LOCATION_LOCAL;
   sf.colorOffset[0]   = offset;
   sf.colorPitch[0]    = pitch;

   // unused color attachments need non-zero pitch
   sf.colorLocation[1] = CELL_GCM_LOCATION_LOCAL;
   sf.colorLocation[2] = CELL_GCM_LOCATION_LOCAL;
   sf.colorLocation[3] = CELL_GCM_LOCATION_LOCAL;
   sf.colorPitch[1]    = 64;
   sf.colorPitch[2]    = 64;
   sf.colorPitch[3]    = 64;

   // no depth buffer (2D only)
   sf.depthFormat      = CELL_GCM_SURFACE_Z16;
   sf.depthLocation    = CELL_GCM_LOCATION_LOCAL;
   sf.depthOffset      = 0;
   sf.depthPitch       = 64;

   sf.type      = CELL_GCM_SURFACE_PITCH;
   sf.antialias = CELL_GCM_SURFACE_CENTER_1;
   sf.width     = w;
   sf.height    = h;
   sf.x         = 0;
   sf.y         = 0;

   cellGcmSetSurface(CTX, &sf);

   float scale[4]  = { w * 0.5f,  h * -0.5f, 0.5f, 0.0f };
   float offs[4]   = { w * 0.5f,  h *  0.5f, 0.5f, 0.0f };
   cellGcmSetViewport(CTX, 0, 0, w, h, 0.0f, 1.0f, scale, offs);
   cellGcmSetScissor(CTX, 0, 0, w, h);
}

// Binds a linear (CELL_GCM_SURFACE_PITCH) RGBA color surface as the render target and sets
// the viewport + scissor to its real w*h. logicalW/logicalH is the coordinate space draw
// calls use on it: the virtual canvas for the display buffer, its own w*h for an offscreen
// texture (offset/pitch 64-byte aligned).
static void setSurface(uint32_t offset, uint32_t pitch, int w, int h, int logicalW, int logicalH)
{
   targetW  = logicalW;
   targetH  = logicalH;
   surfaceW = w;
   surfaceH = h;
   // (V3.1.0 Test 4) shared: only recorded - the next burst writes it with the rest of the target state (emitTargetState)
   targetOffset = offset;
   targetPitch  = pitch;
   if (gfxShareActive) { frameStateValid = 0; return; }
   writeSurfaceState(offset, pitch, w, h);
}

static void setRenderTarget(void)
{
   setSurface(frameOffset[backBuffer], framePitch, screenW, screenH, VIRTUAL_W, VIRTUAL_H);
   targetIsDisplay = 1;
}

// ---- (V3.1.0 Test 4) the shared piece: restart, room, the callback, the target state, bursts, drains, the flip ----

// with the lock held: the next commands from the piece's beginning, once the RSX has read everything up to here (rule 2).
// The flush first; then a wait while get is still at the beginning - after the jump, get there means "back", so the RSX
// must have left the lap's first word before (a lap it had not started would be lost); then one raw jump word (never
// cellGcmSetJumpCommand: its room check could call the callback from in here), put to the beginning, and the context parked -
// begin = current = end - until get is there. 0, or -1 when the RSX did not get that far within timeoutUs: then nothing was
// written (the first wait), or the context stays parked, so that nothing is written until a later restart sees the RSX at the
// beginning, and a flush meanwhile (the block flushes before its restart) can only set put to the beginning again - an old
// lap is never read twice. The 4 KB before offset 0x1000, which every flip calls, are never written
static int gfxCbRestart(uint32_t timeoutUs, int byBlock)
{
   CellGcmControl volatile *control = cellGcmGetControlRegister();
   uint64_t startUs = gfxNowUs();
   if (!cbParked) {
      if (CTX->current == cbBegin) return 0;   // nothing written in this lap: nothing to wait for
      cellGcmFlush(CTX);
      while (control->get == cbBeginOffset) {   // (put lies past the beginning: the flush just set it there)
         if (gfxNowUs() - startUs > timeoutUs) { shared.precheckTimeouts++; return -1; }
         sys_timer_usleep(30);                  // (no more often: the libgcm reference's note on CellGcmControl)
      }
      uint32_t get = control->get, put = control->put;
      uint32_t bytes = (uint32_t)((uint8_t *)CTX->current - (uint8_t *)cbBegin);
      CTX->current[0] = CELL_GCM_JUMP(cbBeginOffset);   // (end lies SHARED_SLACK_BYTES before the piece's end: room for it)
      CTX->current += 1;
      __asm__ volatile("sync" ::: "memory");
      control->put = cbBeginOffset;
      CTX->begin = cbBegin;
      CTX->current = cbBegin;
      CTX->end = cbBegin;
      cbParked = 1;
      if (byBlock) shared.blockLaps++;
      else         shared.appLaps++;
      int n = lapsRecorded;
      if (n < LAPS_LOGGED) {
         LapRecord lap = { get, put, bytes, byBlock };
         lapRecords[n] = lap;
         __sync_synchronize();
         lapsRecorded = n + 1;
      }
   }
   while (control->get != cbBeginOffset) {
      if (gfxNowUs() - startUs > timeoutUs) { shared.restartTimeouts++; return -1; }
      sys_timer_usleep(30);
   }
   CTX->begin = cbBegin;
   CTX->current = cbBegin;
   CTX->end = cbEnd;
   cbParked = 0;
   return 0;
}

// with the lock held: room for `bytes` and the slack - else the piece starts over. A parked piece (the RSX was not back in
// time, and the RSX part is off already) a UI burst tries once a frame and only looks: else every burst of a frame the RSX
// stands still in would wait a whole timeout, and the UI would answer once a second. 0, or -1 (the RSX part braked)
#define PARKED_LOOK_US 2000
static int gfxEnsureRoom(uint32_t bytes, uint32_t timeoutUs, int uiBurst)
{
   if (!cbParked && (uint32_t)((uint8_t *)CTX->end - (uint8_t *)CTX->current) >= bytes + SHARED_SLACK_BYTES) return 0;
   if (cbParked && uiBurst) {
      if (parkRetried) return -1;
      parkRetried = 1;
      timeoutUs = PARKED_LOOK_US;
   }
   if (gfxCbRestart(timeoutUs, 0) == 0) return 0;
   gfxBrake("the RSX did not read the command buffer to its end in time");
   return -1;
}

// libgcm's callback, replaced (rule 2). With room made before every burst it is never reached. Should it be all the same,
// it writes nothing and says no - libgcm then leaves that command out - and the burst goes back whole at its end
// (gfxBurstEnd), so no half-written command (cellGcmSetDrawArrays reserves in parts) ever reaches the RSX. Never CELL_OK
// without the room asked for. The RSX part goes off for the run
static int32_t gfxCbOverflow(struct CellGcmContextData *context, uint32_t words)
{
   (void)context;
   (void)words;
   shared.overflows++;
   burstBroken = 1;
   gfxBrake("libgcm's command buffer callback was reached");
   return CELL_GCM_ERROR_FAILURE;
}

// rule 3, in a burst (room made): at a frame's first, after a flip, a change of target or the block's rows. A flip brings back
// cellGcmInit's state (libgcm overview, "Resetting the RSX Context"); the block's rows leave a B8 target, a viewport of 4096,
// the 8 window clips at its surfaces' size (aw x 280: the app's frame would end 280 lines down), dither off, z-clamp on,
// texture units 0 and 1 on, and vertex arrays 0, 8, 9 and 10 pointing at its memory - 9 and 10 at its vertex ring, which is
// unmapped once it closes (only size 0 stops a fetch). About 0.6 KB
static void emitTargetState(void)
{
   if (surfaceW <= 0 || surfaceH <= 0) {   // (no target recorded yet - a burst before the first frame: nothing to set it to)
      burstBroken = 1;
      return;
   }
   writeSurfaceState(targetOffset, targetPitch, surfaceW, surfaceH);   // the surface, the viewport with the y flip, the scissor
   if (CTX->end - CTX->current >= 24) {   // the window clips: libgcm has only their register numbers (the room is always there)
      for (uint32_t i = 0; i < 8; i++) {
         CTX->current[0] = CELL_GCM_METHOD(CELL_GCM_NV4097_SET_WINDOW_CLIP_HORIZONTAL + 8 * i, 2);
         CTX->current[1] = (uint32_t)(surfaceW - 1) << 16;   // from 0 to w - 1, then the vertical one
         CTX->current[2] = (uint32_t)(surfaceH - 1) << 16;
         CTX->current += 3;
      }
   } else {
      burstBroken = 1;
   }
   cellGcmSetColorMask(CTX, CELL_GCM_COLOR_MASK_R | CELL_GCM_COLOR_MASK_G | CELL_GCM_COLOR_MASK_B | CELL_GCM_COLOR_MASK_A);
   cellGcmSetColorMaskMrt(CTX, 0);
   cellGcmSetDepthTestEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetDepthMask(CTX, CELL_GCM_FALSE);
   cellGcmSetStencilTestEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetAlphaTestEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetLogicOpEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetDitherEnable(CTX, CELL_GCM_TRUE);                                    // (a flip's: what the app's frames had)
   cellGcmSetZMinMaxControl(CTX, CELL_GCM_TRUE, CELL_GCM_FALSE, CELL_GCM_FALSE);   // (... likewise)
   cellGcmSetFragmentProgramGammaEnable(CTX, CELL_GCM_FALSE);
   for (uint32_t unit = 0; unit < 2; unit++)
      cellGcmSetTextureControl(CTX, unit, CELL_GCM_FALSE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   for (uint32_t index = 0; index < 16; index++)   // (each draw of the app's sets its three itself)
      if (index != shaderPosIdx && index != shaderColIdx && index != shaderUvIdx)
         cellGcmSetVertexDataArray(CTX, index, 0, 0, 0, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
}

// one group of the app's commands (rule 1): the lock, room, the target state when a flip, a new target or the block's rows
// may have changed it. 0: write nothing (paused, the frame broken, no lock, no room). Share off: 1, and nothing done
static int gfxBurstBegin(void)
{
   if (!gfxShareActive) return 1;
   if (frameBroken || pauseCount > 0) return 0;
   if (!lockForUi()) { frameBroken = 1; return 0; }
   if (pauseCount > 0) { unlockCmd(); return 0; }   // (a pause began meanwhile: it took the lock and let it go just now)
   if (gfxEnsureRoom(BURST_BYTES, APP_RESTART_US, 1) != 0) { unlockCmd(); frameBroken = 1; return 0; }
   burstStart = CTX->current;
   burstBroken = 0;
   if (!frameStateValid || blockGen != blockGenSeen) {
      emitTargetState();
      frameStateValid = 1;
      blockGenSeen = blockGen;
      shared.resends++;
   }
   return 1;
}

// ... its end. drew: 0 nothing drawn, 1 the batch, 2 a YUV picture. After a draw the app's vertex arrays go off again (the
// block's draws would fetch them), after a picture its texture units too (left on, they would point the block's rows at a
// picture block that may be unmapped by then - vshow.c does the same). Then the state count and the flush. A burst the
// callback was reached in goes back whole - nothing of it was flushed - and the frame writes nothing more. 1, or 0
static int gfxBurstEnd(int drew)
{
   if (!gfxShareActive) return 1;
   if (drew && !burstBroken) {
      cellGcmSetVertexDataArray(CTX, shaderPosIdx, 0, 0, 0, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
      cellGcmSetVertexDataArray(CTX, shaderColIdx, 0, 0, 0, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
      cellGcmSetVertexDataArray(CTX, shaderUvIdx, 0, 0, 0, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
      if (drew == 2)
         for (int i = 0; i < 3; i++)
            cellGcmSetTextureControl(CTX, shaderYuvTexUnit[i], CELL_GCM_FALSE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   }
   if (burstBroken) {
      CTX->current = burstStart;
      frameBroken = 1;
      frameStateValid = 0;
      unlockCmd();
      return 0;
   }
   uint32_t bytes = (uint32_t)((uint8_t *)CTX->current - (uint8_t *)burstStart);
   shared.bursts++;
   shared.bytes += bytes;
   if (bytes > shared.burstMax) shared.burstMax = bytes;
   stateGen++;
   cellGcmFlush(CTX);
   unlockCmd();
   return 1;
}

// the RSX through with every command written so far: a back-end label (255) written under the lock and waited for without
// it, timeoutUs at most. Not held up by a pause - a drain is what a pause waits for. 0, or -1
static int gfxDrain(uint32_t timeoutUs)
{
   uint64_t startUs = gfxNowUs();
   if (!lockForDrain(timeoutUs)) { shared.drainTimeouts++; return -1; }
   if (gfxEnsureRoom(64, timeoutUs < APP_RESTART_US ? timeoutUs : APP_RESTART_US, 0) != 0) {
      unlockCmd();
      shared.drainTimeouts++;
      return -1;
   }
   uint32_t seq = ++drainSeq;
   cellGcmSetWriteBackEndLabel(CTX, DRAIN_LABEL, seq);
   cellGcmFlush(CTX);
   unlockCmd();
   volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(DRAIN_LABEL);
   while ((int32_t)(*label - seq) < 0) {
      if (gfxNowUs() - startUs > timeoutUs) { shared.drainTimeouts++; return -1; }
      sys_timer_usleep(30);
   }
   return 0;
}

static uint64_t flipQueuedUs;                 // (UI thread) when the flip pending now was queued
static uint32_t flipsCounted;                 // ... the confirmed flips the 10-s line has counted

// rule 4, before a flip: the one before it has happened, at most timeoutUs. By the flip handler's count, cross-checked with
// the flip status (reset before every flip): a handler that is late or missed a flip cannot stop the screen - the status says
// the last flip is done, and the count goes on from there (no flip is pending then, so there can never be two). Counts the
// flips and their gaps for the 10-s line. 1: none pending; 0: the last one still is
static int gfxPreviousFlipDone(uint32_t timeoutUs)
{
   uint64_t startUs = gfxNowUs();
   for (;;) {
      uint32_t handled = flipsCompleted;
      if ((int32_t)(handled - flipsConfirmed) > 0)
         flipsConfirmed = (int32_t)(handled - flipsSubmitted) > 0 ? flipsSubmitted : handled;
      if ((int32_t)(flipsSubmitted - flipsConfirmed) <= 0) break;
      if (cellGcmGetFlipStatus() == CELL_GCM_DISPLAY_FLIP_STATUS_DONE) {   // the handler is late, or it missed one
         flipsConfirmed = flipsSubmitted;
         shared.resyncs++;
         break;
      }
      if (gfxNowUs() - startUs >= timeoutUs) return 0;
      sys_timer_usleep(100);
   }
   if (flipsConfirmed != flipsCounted) {
      uint64_t at = (uint64_t)cellGcmGetLastFlipTime();
      if (lastFlipTimeUs && at > lastFlipTimeUs) {
         uint64_t gapUs = at - lastFlipTimeUs;
         uint32_t gap = (uint32_t)(gapUs > 0xFFFFFFFFull ? 0xFFFFFFFFull : gapUs);
         if (gap > shared.gapMaxUs) shared.gapMaxUs = gap;
         if (gap > 25000) shared.gaps25++;
         if (gap > 41700) shared.gaps42++;
      }
      if (at) lastFlipTimeUs = at;
      shared.flips += flipsConfirmed - flipsCounted;
      flipsCounted = flipsConfirmed;
   }
   return 1;
}

// a frame without a flip waits for the next vertical blank instead (1 ms steps, 20 ms at most): the UI loop, at priority 1001,
// never spins over the block's threads and the player's
static void sleepToNextVblank(void)
{
   uint64_t vblank = cellGcmGetVBlankCount();
   for (int ms = 0; ms < 20 && cellGcmGetVBlankCount() == vblank; ms++) sys_timer_usleep(1000);
}

// the lines that could not be written where they arose (under the lock, or on another thread), and the 10-s line (UI thread)
static void sharedHousekeeping(uint64_t nowUs)
{
   gfxLogBrake();
   if (lockError && !lockErrorLogged) {
      lockErrorLogged = 1;
      logError("[gfx] shared: the command buffer's lock failed with 0x%08x\n", (unsigned)lockError);
   }
   while (lapsLogged < lapsRecorded) {
      const LapRecord *lap = &lapRecords[lapsLogged++];
      logInfo("[gfx] shared: lap %d of the command buffer, by the %s: %u KB, get 0x%x and put 0x%x at the jump\n", lapsLogged,
              lap->byBlock ? "block" : "app", lap->bytes >> 10, lap->get, lap->put);
   }
   if (flipBurstBytes && !flipBurstLogged) {
      flipBurstLogged = 1;
      logInfo("[gfx] shared: a flip takes %u bytes of commands\n", flipBurstBytes);
   }
   if (!shared.sinceUs) { shared.sinceUs = nowUs; return; }
   if (nowUs - shared.sinceUs < SHARED_STATS_US) return;
   // integers only: this printf has no floating point
   logInfo("[gfx] shared: %u flips in %u s (longest gap %u ms, %u over 25 ms, %u over 41.7 ms, %u by the flip status), %u frames "
           "without one; %u bursts, %u KB, the largest %u bytes, %u with the state again; laps: %u by the app, %u by the block; "
           "timeouts: %u pre-check, %u restart, %u drain; %u overflow calls; the UI waited %u times for the lock (at most %u us), "
           "the block held it %u us at most%s%s\n", shared.flips, (unsigned)((nowUs - shared.sinceUs) / 1000000),
           shared.gapMaxUs / 1000, shared.gaps25, shared.gaps42, shared.resyncs, shared.skipped, shared.bursts,
           (unsigned)(shared.bytes >> 10), shared.burstMax, shared.resends, shared.appLaps, shared.blockLaps,
           shared.precheckTimeouts, shared.restartTimeouts, shared.drainTimeouts, shared.overflows, shared.uiWaits,
           shared.uiWaitMaxUs, shared.blockHoldMaxUs, cbParked ? "; parked" : "", rsxBrake ? "; the RSX part is off" : "");
   memset(&shared, 0, sizeof shared);
   shared.sinceUs = nowUs;
}

// endGfxFrame, shared (rule 4): the flip without a wait in the command stream. It waits until the flip before it has happened
// (at most 100 ms - else this frame goes without one), the flip status is reset, the flip is one burst. A frame paused, broken
// or without its flip waits for the next vertical blank instead. Then the log lines kept back, outside the lock
static void endGfxFrameShared(void)
{
   uint64_t startUs = gfxNowUs();
   if (pauseCount > 0 && startUs - pauseSinceUs > PAUSE_MAX_US) {
      pauseCount = 0;
      logWarn("[gfx] shared: drawing was paused for %d s and nothing let it go on - it goes on now\n", (int)(PAUSE_MAX_US / 1000000));
   }
   int flipped = 0;
   if (pauseCount <= 0 && !frameBroken) {
      if (gfxPreviousFlipDone(FLIP_WAIT_US)) {
         cellGcmResetFlipStatus();   // (never while a flip may happen: none is pending now)
         if (gfxBurstBegin()) {
            uint32_t *before = CTX->current;
            int32_t rc = cellGcmSetFlip(CTX, (uint8_t)backBuffer);
            uint32_t bytes = (uint32_t)((uint8_t *)CTX->current - (uint8_t *)before);
            if (gfxBurstEnd(0) && rc == CELL_OK && bytes) {
               flipsSubmitted++;
               flipQueuedUs = gfxNowUs();
               backBuffer = (backBuffer + 1) % FRAME_COUNT;
               if (!flipBurstBytes) flipBurstBytes = bytes;
               flipped = 1;
            }
         }
      } else if (!flipStallLogged && startUs - flipQueuedUs > FLIP_STALL_US) {
         CellGcmControl volatile *control = cellGcmGetControlRegister();
         flipStallLogged = 1;
         logWarn("[gfx] shared: flip %u has not happened %u ms after it was queued - get 0x%x, put 0x%x, labels 200 %u, 210 %u, "
                 "211 %u, 212 %u\n", flipsSubmitted, (unsigned)((startUs - flipQueuedUs) / 1000), control->get, control->put,
                 *(volatile uint32_t *)cellGcmGetLabelAddress(200), *(volatile uint32_t *)cellGcmGetLabelAddress(210),
                 *(volatile uint32_t *)cellGcmGetLabelAddress(211), *(volatile uint32_t *)cellGcmGetLabelAddress(212));
         gfxBrake("a flip did not happen within 2 s");
      }
   }
   frameStateValid = 0;   // (a flip brings back cellGcmInit's state; without one the next frame sets it again all the same)
   if (!flipped) {
      shared.skipped++;
      sleepToNextVblank();
   }
   lastFlipWaitUs = gfxNowUs() - startUs;
   sharedHousekeeping(gfxNowUs());
}

// rule 1's lock: an lv2 mutex, FIFO (SYS_SYNC_PRIORITY_INHERIT is "not implemented"), not recursive
static int createCmdLock(void)
{
   sys_mutex_attribute_t attr;
   sys_mutex_attribute_initialize(attr);
   attr.attr_protocol = SYS_SYNC_FIFO;
   attr.attr_recursive = SYS_SYNC_NOT_RECURSIVE;
   memcpy(attr.name, "teegfx", 7);
   int rc = sys_mutex_create(&cmdLock, &attr);
   cmdLockOk = rc == CELL_OK;
   if (!cmdLockOk) logError("[gfx] shared: no lock for the command buffer (0x%08x) - it is not shared\n", (unsigned)rc);
   return cmdLockOk;
}

// rule 2, after cellGcmInit(SHARED_CMD_BYTES): libgcm's first segment has to begin at offset 0x1000 of the host block (behind
// the 4 KB every flip calls), with its end inside the buffer. Then the end moves to the piece's end and the callback is ours,
// so libgcm's segment switch never runs. Anything else, and libgcm's segments stay with the flip path of gfx-share=off
static void setUpSharedBuffer(uint8_t *host)
{
   uint32_t *begin = CTX->begin, *current = CTX->current, *end = CTX->end;
   uint32_t beginOffset = 0;
   int fits = (uint8_t *)begin == host + CELL_GCM_INIT_STATE_OFFSET && cellGcmAddressToOffset(begin, &beginOffset) == CELL_OK &&
              beginOffset == CELL_GCM_INIT_STATE_OFFSET && begin <= current && current < end &&
              (uint8_t *)end <= host + SHARED_CMD_BYTES;
   if (!fits) {
      logWarn("[gfx] command buffer: libgcm left begin at +0x%x (offset 0x%x), current at +0x%x, end at +0x%x - not the layout "
              "the sharing needs: libgcm's segments stay, the buffer is not shared\n", (unsigned)((uint8_t *)begin - host),
              (unsigned)beginOffset, (unsigned)((uint8_t *)current - host), (unsigned)((uint8_t *)end - host));
      return;
   }
   if (fileExists(RSX_MARKER)) {   // (setGfxRsxActive)
      rsxCrashOff = 1;
      deleteFile(RSX_MARKER);
      logWarn("[gfx] shared: the last run ended while the RSX predicted for TEE VP9 (%s was there) - vp9-rsx is off for this "
              "run\n", RSX_MARKER);
   }
   uiThread = gfxThread();
   cbBegin = begin;
   cbBeginOffset = beginOffset;
   cbEnd = (uint32_t *)(host + (gfxSmallCb ? CELL_GCM_INIT_STATE_OFFSET + SHARED_SMALL_BYTES : SHARED_CMD_BYTES - SHARED_SLACK_BYTES));
   CTX->end = cbEnd;
   CTX->callback = gfxCbOverflow;
   *(volatile uint32_t *)cellGcmGetLabelAddress(DRAIN_LABEL) = 0;
   gfxShareActive = 1;
   logInfo("[gfx] command buffer: one piece of %u KB from offset 0x%x (libgcm's first segment ended at +0x%x), shared%s; %u MB "
           "of IO address space\n", (unsigned)(((uint8_t *)cbEnd - (uint8_t *)cbBegin) >> 10), (unsigned)cbBeginOffset,
           (unsigned)((uint8_t *)end - host), gfxSmallCb ? " (rsx-test=small-cb)" : "", (unsigned)(cellGcmGetMaxIoMapSize() >> 20));
}

// the block's RSX memory (tv9h_local_alloc): 2 MB of the app's, taken once at the start, with a lock of its own
static void setUpLocalPool(void)
{
   sys_lwmutex_attribute_t attr;
   sys_lwmutex_attribute_initialize(attr);
   localPool = (uint8_t *)allocateVram(LOCAL_POOL_BYTES, 128);
   if (localPool && sys_lwmutex_create(&localLock, &attr) != CELL_OK) {
      freeVram(localPool);
      localPool = NULL;
   }
   if (!localPool) logWarn("[gfx] shared: no 2 MB of RSX memory for TEE VP9's RSX part - it stays off\n");
}

int initGfx(GfxVsync vsync)
{
   int ret;

   if (initialized) {
      return 0;
   }

   cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
   cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);

   // allocate host-side command buffer pool
   void *hostAddr = memalign(1024 * 1024, HOST_MEM_SIZE);
   if (!hostAddr) {
      logError("[gfx] memalign host failed\n");
      return -1;
   }

   // (V3.1.0 Test 4) gfx-share=on: the lock first, and the command buffer 960 KB of the same 1 MB (see "one command buffer")
   int share = gfxShareWanted && createCmdLock();
   ret = cellGcmInit(share ? SHARED_CMD_BYTES : CMD_BUFFER_SIZE, HOST_MEM_SIZE, hostAddr);
   if (ret < 0) {
      logError("[gfx] cellGcmInit failed: 0x%08x\n", ret);
      return ret;
   }
   if (share) setUpSharedBuffer((uint8_t *)hostAddr);

   // read current video mode
   CellVideoOutState videoState;
   ret = cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &videoState);
   if (ret < 0) return ret;

   CellVideoOutResolution res;
   ret = cellVideoOutGetResolution(videoState.displayMode.resolutionId, &res);
   if (ret < 0) return ret;

   screenW = res.width;
   screenH = res.height;
   framePitch = cellGcmGetTiledPitchSize(cellGcmAlign(CELL_GCM_ZCULL_ALIGN_WIDTH, screenW) * BYTES_PER_PIXEL);

   // configure video output
   CellVideoOutConfiguration videoCfg;
   memset(&videoCfg, 0, sizeof(videoCfg));
   videoCfg.resolutionId = videoState.displayMode.resolutionId;
   videoCfg.format       = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
   videoCfg.pitch        = framePitch;

   ret = cellVideoOutConfigure(CELL_VIDEO_OUT_PRIMARY, &videoCfg, 0, 0);
   if (ret < 0) {
      logError("[gfx] cellVideoOutConfigure: 0x%08x\n", ret);
      return ret;
   }

   setGfxVsync(vsync);

   // set up vram allocator
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   vramBase = (uint8_t *)config.localAddress;
   vramSize = (size_t)config.localSize;

   // allocate frame buffers in vram
   uint32_t bufferHeight = cellGcmAlign(CELL_GCM_ZCULL_ALIGN_HEIGHT, screenH);
   uint32_t colorSize    = framePitch * bufferHeight;

   for (int i = 0; i < FRAME_COUNT; ++i) {
      void *addr = allocateVram(colorSize, CELL_GCM_TILE_ALIGN_OFFSET);
      if (!addr) {
         return -1;
      }

      ret = cellGcmAddressToOffset(addr, &frameOffset[i]);
      if (ret < 0) {
         logError("[gfx] cellGcmAddressToOffset[%d]: 0x%08x\n", i, ret);
         return ret;
      }

      cellGcmSetTileInfo(i, CELL_GCM_LOCATION_LOCAL, frameOffset[i], colorSize, framePitch, CELL_GCM_COMPMODE_DISABLED, 0, 0);
      cellGcmBindTile(i);

      ret = cellGcmSetDisplayBuffer(i, frameOffset[i], framePitch, screenW, screenH);
      if (ret < 0) {
         logError("[gfx] cellGcmSetDisplayBuffer[%d]: 0x%08x\n", i, ret);
         return ret;
      }
   }

   backBuffer = 0;
   flipsSubmitted = 0;
   flipsCompleted = 0;
   lastFlipUs = 0;
   flipGaps25 = flipGaps42 = flipMaxGapUs = 0;
   vblanksMissed = 0;
   lastFlipVblank = 0;
   lastFlipTimeUs = 0;
   uint16_t rates = videoState.displayMode.refreshRates;   // one vblank of the screen, for the distance between flips
   screenVblankUs = (rates & CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ) ? 16683 : (rates & CELL_VIDEO_OUT_REFRESH_RATE_60HZ) ? 16667
                  : (rates & CELL_VIDEO_OUT_REFRESH_RATE_50HZ) ? 20000 : (rates & CELL_VIDEO_OUT_REFRESH_RATE_30HZ) ? 33333 : 0;
   cellGcmSetFlipHandler(onGfxFlip);   // retires in-flight flips so endGfxFrame need not block on them
   initialized = 1;

   // init shaders
   shaderVp = (CGprogram)&_binary_vpshader_vpo_start;
   cellGcmCgInitProgram(shaderVp);
   uint32_t vpSize;
   cellGcmCgGetUCode(shaderVp, &shaderVpUcode, &vpSize);

   shaderFp = (CGprogram)&_binary_fpshader_fpo_start;
   cellGcmCgInitProgram(shaderFp);
   void *fpUcode;
   uint32_t fpSize;
   cellGcmCgGetUCode(shaderFp, &fpUcode, &fpSize);

   shaderFpUcode = allocateVram(fpSize, 64);
   if (!shaderFpUcode) return -1;
   memcpy(shaderFpUcode, fpUcode, fpSize);
   cellGcmAddressToOffset(shaderFpUcode, &shaderFpOffset);

   CGparameter posParam = cellGcmCgGetNamedParameter(shaderVp, "position");
   CGparameter colParam = cellGcmCgGetNamedParameter(shaderVp, "color");
   CGparameter uvParam  = cellGcmCgGetNamedParameter(shaderVp, "texcoord");
   shaderPosIdx = cellGcmCgGetParameterResource(shaderVp, posParam) - CG_ATTR0;
   shaderColIdx = cellGcmCgGetParameterResource(shaderVp, colParam) - CG_ATTR0;
   shaderUvIdx  = cellGcmCgGetParameterResource(shaderVp, uvParam)  - CG_ATTR0;

   CGparameter texParam = cellGcmCgGetNamedParameter(shaderFp, "tex");
   shaderTexUnit = cellGcmCgGetParameterResource(shaderFp, texParam) - CG_TEXUNIT0;

   // yuv video shader
   shaderFpYuv = (CGprogram)&_binary_fpshader_yuv_fpo_start;
   cellGcmCgInitProgram(shaderFpYuv);
   void *fpYuvUcode;
   uint32_t fpYuvSize;
   cellGcmCgGetUCode(shaderFpYuv, &fpYuvUcode, &fpYuvSize);
   void *fpYuvVram = allocateVram(fpYuvSize, 64);
   if (!fpYuvVram) return -1;
   memcpy(fpYuvVram, fpYuvUcode, fpYuvSize);
   cellGcmAddressToOffset(fpYuvVram, &shaderFpYuvOffset);

   static const char *planeNames[3] = { "texY", "texU", "texV" };
   for (int i = 0; i < 3; i++) {
      CGparameter planeParam = cellGcmCgGetNamedParameter(shaderFpYuv, planeNames[i]);
      shaderYuvTexUnit[i] = cellGcmCgGetParameterResource(shaderFpYuv, planeParam) - CG_TEXUNIT0;
   }
   shaderYuvChromaXf = cellGcmCgGetNamedParameter(shaderFpYuv, "chromaXf");
   yuvChromaXfValid = 0;

   // 1x1 white texture (64-byte pitch minimum for RSX)
   uint32_t *whitePix = (uint32_t *)allocateVram(64, 64);
   if (!whitePix) return -1;
   *whitePix = COLOR_WHITE;
   cellGcmAddressToOffset(whitePix, &whiteTexOffset);

   // allocate quad batch buffer in vram
   for (int i = 0; i < FRAME_COUNT; ++i) {
      batchVerts[i] = (GfxVertex *)allocateVram(MAX_VERTS * sizeof(GfxVertex), 128);
      if (!batchVerts[i]) return -1;
      cellGcmAddressToOffset(&batchVerts[i][0].x, &batchOffset[i]);
      cellGcmAddressToOffset(&batchVerts[i][0].rgba, &batchColOffset[i]);
      cellGcmAddressToOffset(&batchVerts[i][0].u, &batchUvOffset[i]);
   }
   if (gfxShareActive) setUpLocalPool();   // (V3.1.0 Test 4) the block's RSX memory

   logInfo("[gfx] ready: %d x %d (virtual %d x %d), pitch=%u, vram_used=%zu/%zu\n", screenW, screenH, VIRTUAL_W, VIRTUAL_H, framePitch, getUsedVram(), vramSize);
   return 0;
}

void termGfx(void)
{
   if (!initialized) {
      return;
   }
   if (gfxShareActive) {
      // (V3.1.0 Test 4) shared: no wait for the flip in the command stream and no cellGcmFinish without a bound - the RSX part
      // off, at most 100 ms for the flip under way, a drain of at most 0.5 s
      gfxLogBrake();     // (a brake from before, if its line is still due)
      gfxBrake("the app ends");
      brakeLogged = 1;   // (no line for this one)
      gfxPreviousFlipDone(FLIP_WAIT_US);
      if (gfxDrain(DRAIN_EXIT_US) != 0) logWarn("[gfx] shared: the RSX did not finish its commands within 0.5 s at the end\n");
      localPool = NULL;   // (the VRAM goes with the allocator's list below)
   } else {
      // wait for last flip to complete
      cellGcmSetWaitFlip(CTX);
      cellGcmFinish(CTX, 1);
   }

   // release all allocator metadata. The VRAM region itself is owned by libgcm;
   // dropping the block list (incl. the permanent frame/shader/batch blocks)
   // resets the allocator so a later initGfx() rebuilds it from scratch.
   VramBlock *block = blockListHead;
   while (block) {
      VramBlock *next = block->next;
      free(block);
      block = next;
   }
   blockListHead = NULL;
   vramAllocated = 0;

   initialized = 0;
}

void beginGfxFrame(void)
{
   if (!initialized) return;

   if (gfxShareActive) {   // (V3.1.0 Test 4) a new frame: it may write again, and try a parked piece once
      frameBroken = 0;
      parkRetried = 0;
   }
   freeRetiredTextures();   // the frame that named them has been shown
   setRenderTarget();   // binds the display buffer + sets viewport/scissor to the screen

   if (!gfxShareActive) {   // (V3.1.0 Test 4) shared: emitTargetState sets these, in the frame's first burst
      cellGcmSetColorMask(CTX, CELL_GCM_COLOR_MASK_R | CELL_GCM_COLOR_MASK_G | CELL_GCM_COLOR_MASK_B | CELL_GCM_COLOR_MASK_A);
      cellGcmSetColorMaskMrt(CTX, 0);

      cellGcmSetDepthTestEnable(CTX, CELL_GCM_FALSE);
      cellGcmSetFragmentProgramGammaEnable(CTX, CELL_GCM_FALSE);
   }

   batchVertCount = 0;
   batchFlushStart = 0;
   frameDroppedCalls = 0;
   batchTexOffset = whiteTexOffset;
   batchTexW = 1;
   batchTexH = 1;
   batchTexPitch = 64;
   batchTexLinear = 0;
}

void clearGfx(uint32_t argb)
{
   if (!initialized) return;
   if (!gfxBurstBegin()) return;   // (V3.1.0 Test 4) shared: one burst; share off: nothing more than before
   cellGcmSetScissor(CTX, 0, 0, surfaceW, surfaceH);
   cellGcmSetClearColor(CTX, argb);
   cellGcmSetClearSurface(CTX, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
   gfxBurstEnd(0);
}

static uint32_t argbToRgba(uint32_t argb)
{
   return ((argb & 0x00FF0000) << 8) | ((argb & 0x0000FF00) << 8) |
         ((argb & 0x000000FF) << 8) | ((argb >> 24) & 0xFF);
}

// convert a pixel coordinate to clip space [-1,1] within the current render target
static float toClipX(int px) { return (float)px / (float)targetW * 2.0f - 1.0f; }
static float toClipY(int py) { return 1.0f - (float)py / (float)targetH * 2.0f; }

// ensure we're batching against the 1x1 white texture (used by all color-only draws)
static void ensureWhiteTex(void)
{
   if (batchTexOffset != whiteTexOffset) {
      flushBatch();
      batchTexOffset = whiteTexOffset;
      batchTexW = 1;
      batchTexH = 1;
      batchTexPitch = 64;
      batchTexLinear = 0;
   }
}

int getGfxVertexBudget(void) { return MAX_VERTS; }
int getGfxVerticesUsed(void) { return batchVertCount; }

// batch is full for this frame: drop the call whole. counted, not logged here -
// the frame reports itself in endGfxFrame, so one overflowing screen cannot bury
// the log under a line per dropped call.
static int batchWouldOverflow(int vertsNeeded)
{
   if (batchVertCount + vertsNeeded <= MAX_VERTS) return 0;
   frameDroppedCalls++;
   return 1;
}

// a frame that is merely close to the cap is one screen element away from
// silently losing draws, so the warning comes while there is still headroom
// rather than after the picture has already broken. rate limited, because a
// heavy screen is heavy on every frame and the log is a file on disk.
#define BUDGET_WARN_PERCENT     80
#define BUDGET_WARN_INTERVAL_US 1000000

static void reportFrameBudget(void)
{
   int usedPercent = batchVertCount * 100 / MAX_VERTS;
   if (frameDroppedCalls == 0 && usedPercent < BUDGET_WARN_PERCENT) return;

   static uint64_t lastWarnUs;
   uint64_t now = sys_time_get_system_time();
   if (now - lastWarnUs < BUDGET_WARN_INTERVAL_US) return;
   lastWarnUs = now;

   if (frameDroppedCalls > 0)
      logWarn("[gfx] frame ran out of vertices (cap %d): %d draw calls dropped\n", MAX_VERTS, frameDroppedCalls);
   else
      logWarn("[gfx] frame used %d of %d vertices (%d%%)\n", batchVertCount, MAX_VERTS, usedPercent);
}

void fillGfxRectangle(int x, int y, int w, int h, uint32_t argb)
{
   if (!initialized) return;
   ensureWhiteTex();
   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   if (x < 0) { w += x; x = 0; }
   if (y < 0) { h += y; y = 0; }
   if (x + w > targetW) w = targetW - x;
   if (y + h > targetH) h = targetH - y;
   if (w <= 0 || h <= 0) return;

   float x0 = toClipX(x),     y0 = toClipY(y);
   float x1 = toClipX(x + w), y1 = toClipY(y + h);
   uint32_t rgba = argbToRgba(argb);

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ x0, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[1] = (GfxVertex){ x1, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[2] = (GfxVertex){ x0, y1, 0.0f, rgba, 0.0f, 0.0f };
   v[3] = (GfxVertex){ x1, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[4] = (GfxVertex){ x1, y1, 0.0f, rgba, 0.0f, 0.0f };
   v[5] = (GfxVertex){ x0, y1, 0.0f, rgba, 0.0f, 0.0f };

   batchVertCount += VERTS_PER_RECT;
}

// metro-style border: four edges drawn inward, so the outline stays within the w x h bounds.
void strokeGfxRectangle(int x, int y, int w, int h, int thickness, uint32_t argb)
{
   if (!initialized || thickness <= 0 || w <= 0 || h <= 0) return;
   if (batchWouldOverflow(VERTS_PER_STROKED_RECT)) return;   // all four edges or none
   if (thickness * 2 > w) thickness = w / 2;
   if (thickness * 2 > h) thickness = h / 2;

   fillGfxRectangle(x, y, w, thickness, argb);                              // top
   fillGfxRectangle(x, y + h - thickness, w, thickness, argb);              // bottom
   fillGfxRectangle(x, y + thickness, thickness, h - 2 * thickness, argb);  // left
   fillGfxRectangle(x + w - thickness, y + thickness, thickness, h - 2 * thickness, argb);  // right
}

// metro panel: filled interior with a border on top (the flat replacement for a rounded 9-slice sprite).
void drawGfxBox(int x, int y, int w, int h, int thickness, uint32_t fill, uint32_t border)
{
   if (batchWouldOverflow(VERTS_PER_RECT + VERTS_PER_STROKED_RECT)) return;   // fill and border together
   fillGfxRectangle(x, y, w, h, fill);
   strokeGfxRectangle(x, y, w, h, thickness, border);
}

// vertices are in pixel coords (origin top-left), converted to clip space internally.
void drawGfxTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
   if (!initialized) return;
   ensureWhiteTex();
   if (batchWouldOverflow(VERTS_PER_TRIANGLE)) return;

   float invW = 2.0f / (float)targetW;
   float invH = 2.0f / (float)targetH;
   float nx0 = x0 * invW - 1.0f, ny0 = 1.0f - y0 * invH;
   float nx1 = x1 * invW - 1.0f, ny1 = 1.0f - y1 * invH;
   float nx2 = x2 * invW - 1.0f, ny2 = 1.0f - y2 * invH;

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ nx0, ny0, 0.0f, argbToRgba(c0), 0.0f, 0.0f };
   v[1] = (GfxVertex){ nx1, ny1, 0.0f, argbToRgba(c1), 0.0f, 0.0f };
   v[2] = (GfxVertex){ nx2, ny2, 0.0f, argbToRgba(c2), 0.0f, 0.0f };
   batchVertCount += VERTS_PER_TRIANGLE;
}

// thick line via two triangles: builds a quad oriented along the line, expanded
// by thickness/2 in the perpendicular direction. degenerate (zero-length) lines
// are skipped.
void drawGfxLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb)
{
   if (!initialized || thickness <= 0) return;
   if (batchWouldOverflow(VERTS_PER_LINE)) return;   // both triangles or neither, so a line never tapers
   float dx = (float)(x1 - x0);
   float dy = (float)(y1 - y0);
   float len = sqrtf(dx * dx + dy * dy);
   if (len <= 0.0f) return;

   float t  = (float)thickness * 0.5f;
   float px = -dy / len * t;
   float py =  dx / len * t;

   float ax = (float)x0 + px, ay = (float)y0 + py;
   float bx = (float)x0 - px, by = (float)y0 - py;
   float cx = (float)x1 + px, cy = (float)y1 + py;
   float dx2 = (float)x1 - px, dy2 = (float)y1 - py;

   drawGfxTriangle(ax, ay, argb, bx, by, argb, cx, cy, argb);
   drawGfxTriangle(bx, by, argb, dx2, dy2, argb, cx, cy, argb);
}

// a circle is a fan of triangles, so its cost is its smoothness. segments finer
// than a pixel of outline are invisible and were the single largest waste in the
// tree (96 four-pixel bar caps at 24 segments each), so the count follows the
// radius: a 2-pixel dot gets 6, anything from 24 pixels up gets the full 24.
#define CIRCLE_MIN_SEGMENTS 6
#define CIRCLE_MAX_SEGMENTS 24

static int getCircleSegments(int radius)
{
   if (radius < CIRCLE_MIN_SEGMENTS) return CIRCLE_MIN_SEGMENTS;
   if (radius > CIRCLE_MAX_SEGMENTS) return CIRCLE_MAX_SEGMENTS;
   return radius;
}

void fillGfxCircle(int cx, int cy, int r, uint32_t argb)
{
   if (!initialized || r <= 0) return;
   ensureWhiteTex();
   int segments = getCircleSegments(r);
   if (batchWouldOverflow(segments * VERTS_PER_TRIANGLE)) return;

   uint32_t rgba = argbToRgba(argb);
   float fcx = toClipX(cx);
   float fcy = toClipY(cy);
   float step = 6.2831853f / (float)segments;
   float prevX = toClipX(cx + r);
   float prevY = fcy;

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   for (int i = 1; i <= segments; ++i) {
      float angle = step * (float)i;
      float nx = toClipX(cx + (int)(cosf(angle) * (float)r));
      float ny = toClipY(cy + (int)(sinf(angle) * (float)r));
      v[0] = (GfxVertex){ fcx, fcy, 0.0f, rgba, 0.0f, 0.0f };
      v[1] = (GfxVertex){ prevX, prevY, 0.0f, rgba, 0.0f, 0.0f };
      v[2] = (GfxVertex){ nx, ny, 0.0f, rgba, 0.0f, 0.0f };
      v += 3;
      prevX = nx;
      prevY = ny;
   }
   batchVertCount += segments * VERTS_PER_TRIANGLE;
}

static void flushBatch(void)
{
   int count = batchVertCount - batchFlushStart;
   if (count <= 0) return;
   // (V3.1.0 Test 4) shared: one burst (never inside another one); one that cannot be written drops its draw
   if (!gfxBurstBegin()) { batchFlushStart = batchVertCount; return; }

   cellGcmSetVertexProgram(CTX, shaderVp, shaderVpUcode);
   cellGcmSetFragmentProgram(CTX, shaderFp, shaderFpOffset);

   // point vertex arrays at the flush start offset
   uint32_t startByte = batchFlushStart * sizeof(GfxVertex);
   cellGcmSetVertexDataArray(CTX, shaderPosIdx, 0, sizeof(GfxVertex), 3, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderColIdx, 0, sizeof(GfxVertex), 4, CELL_GCM_VERTEX_UB, CELL_GCM_LOCATION_LOCAL, batchColOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderUvIdx, 0, sizeof(GfxVertex), 2, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchUvOffset[backBuffer] + startByte);

   // bind current batch texture
   CellGcmTexture tex;
   memset(&tex, 0, sizeof(tex));
   tex.format    = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
   tex.mipmap    = 1;
   tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
   tex.cubemap   = CELL_GCM_FALSE;
   tex.remap     = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY, CELL_GCM_TEXTURE_REMAP_FROM_A, CELL_GCM_TEXTURE_REMAP_FROM_R, CELL_GCM_TEXTURE_REMAP_FROM_G, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP);
   tex.width     = batchTexW;
   tex.height    = batchTexH;
   tex.depth     = 1;
   tex.pitch     = batchTexPitch;
   tex.location  = CELL_GCM_LOCATION_LOCAL;
   tex.offset    = batchTexOffset;
   cellGcmSetTexture(CTX, shaderTexUnit, &tex);
   cellGcmSetTextureControl(CTX, shaderTexUnit, CELL_GCM_TRUE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   cellGcmSetTextureAddress(CTX, shaderTexUnit, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
   int minFilt = batchTexLinear ? CELL_GCM_TEXTURE_LINEAR : CELL_GCM_TEXTURE_NEAREST;
   int magFilt = batchTexLinear ? CELL_GCM_TEXTURE_LINEAR : CELL_GCM_TEXTURE_NEAREST;
   cellGcmSetTextureFilter(CTX, shaderTexUnit, 0, minFilt, magFilt, CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);

   cellGcmSetInvalidateTextureCache(CTX, CELL_GCM_INVALIDATE_TEXTURE);

   cellGcmSetCullFaceEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetBlendEnable(CTX, CELL_GCM_TRUE);
   cellGcmSetBlendFunc(CTX, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA);

   // (V3.1.0 Test 4) shared: the RSX may still draw the batch before while this one's vertices are written beside it - the
   // draw itself only invalidates the vertex file, not the cache (gcm_implementation_sub.h SetDrawArrays)
   if (gfxShareActive) cellGcmSetInvalidateVertexCache(CTX);
   cellGcmSetDrawArrays(CTX, CELL_GCM_PRIMITIVE_TRIANGLES, 0, count);

   cellGcmSetBlendEnable(CTX, CELL_GCM_FALSE);
   gfxBurstEnd(1);

   batchFlushStart = batchVertCount;
}

void drawGfxTexture(int x, int y, int w, int h, GfxTexture tex, float u0, float v0, float u1, float v1, uint32_t tint, GfxFilter filter)
{
   if (!initialized || tex.w == 0 || tex.h == 0) return;

   // nearest is crisp only at 1:1. on a sub-1080p screen every draw is shrunk from the virtual
   // canvas, so force smooth filtering there or text and sprites go blocky. offscreen targets
   // stay 1:1 (their logical size is their real size) and keep the caller's choice.
   if (filter == GFX_FILTER_NEAREST && targetIsDisplay && screenW != VIRTUAL_W)
      filter = GFX_FILTER_LINEAR;

   // flush if texture or filter mode changes
   if (tex.offset != batchTexOffset || filter != batchTexLinear) {
      flushBatch();
      batchTexOffset = tex.offset;
      batchTexW = tex.w;
      batchTexH = tex.h;
      batchTexPitch = tex.pitch;
      batchTexLinear = filter;
   }

   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   float cx0 = (float)x / (float)targetW * 2.0f - 1.0f;
   float cy0 = 1.0f - (float)y / (float)targetH * 2.0f;
   float cx1 = (float)(x + w) / (float)targetW * 2.0f - 1.0f;
   float cy1 = 1.0f - (float)(y + h) / (float)targetH * 2.0f;

   uint32_t rgba = argbToRgba(tint);

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ cx0, cy0, 0.0f, rgba, u0, v0 };
   v[1] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
   v[2] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };
   v[3] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
   v[4] = (GfxVertex){ cx1, cy1, 0.0f, rgba, u1, v1 };
   v[5] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };

   batchVertCount += VERTS_PER_RECT;
}

// ============================================================================
// video frame path: YUV 4:2:0 planar frames decoded straight into RSX-visible
// main memory, drawn zero-copy with the yuv shader doing the color conversion
// ============================================================================

void *allocGfxVideoBuffer(size_t size)
{
   size_t mappedSize = (size + 0xFFFFF) & ~(size_t)0xFFFFF;   // MapMainMemory works in 1MB units
   void *buffer = memalign(1024 * 1024, mappedSize);
   if (!buffer) return 0;
   uint32_t ioOffset;
   if (cellGcmMapMainMemory(buffer, mappedSize, &ioOffset) != CELL_OK) {
      logError("[gfx] MapMainMemory %zu KB failed\n", mappedSize / 1024);
      free(buffer);
      return 0;
   }
   return buffer;
}

void freeGfxVideoBuffer(void *buffer)
{
   if (!buffer) return;
   cellGcmUnmapEaIoAddress(buffer);
   free(buffer);
}

// binds one Y/U/V plane as a single-channel linear texture the shader reads via .x
static void setYuvPlaneTexture(uint32_t unit, uint32_t offset, int width, int height, int pitch)
{
   CellGcmTexture tex;
   memset(&tex, 0, sizeof(tex));
   tex.format    = CELL_GCM_TEXTURE_B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
   tex.mipmap    = 1;
   tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
   tex.cubemap   = CELL_GCM_FALSE;
   tex.remap     = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP);
   tex.width     = width;
   tex.height    = height;
   tex.depth     = 1;
   tex.pitch     = pitch;
   tex.location  = CELL_GCM_LOCATION_MAIN;
   tex.offset    = offset;
   cellGcmSetTexture(CTX, unit, &tex);
   cellGcmSetTextureControl(CTX, unit, CELL_GCM_TRUE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   cellGcmSetTextureAddress(CTX, unit, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
   cellGcmSetTextureFilter(CTX, unit, 0, CELL_GCM_TEXTURE_LINEAR, CELL_GCM_TEXTURE_LINEAR, CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
}

void drawGfxYuvFrame(int x, int y, int w, int h, const void *yuvPlanes, int frameW, int frameH)
{
   if (!yuvPlanes || frameW <= 0 || frameH <= 0) return;
   // packed: Y, then U, then V, each plane as wide as its picture - a picture without a border
   uint32_t lumaBytes = (uint32_t)(frameW * frameH);
   const uint8_t *base = (const uint8_t *)yuvPlanes;
   GfxYuvPlanes planes = { base, base + lumaBytes, base + lumaBytes + lumaBytes / 4, frameW, frameW / 2,
                           frameW, frameH, frameW / 2, frameH / 2, 0, 0, frameW, frameH, 0, 0 };
   drawGfxYuvPlanes(x, y, w, h, &planes);
}

void drawGfxYuvPlanes(int x, int y, int w, int h, const GfxYuvPlanes *p)
{
   if (!initialized || !p || !p->y || !p->u || !p->v || p->planeW <= 0 || p->planeH <= 0 ||
       p->planeWC <= 0 || p->planeHC <= 0 || p->width <= 0 || p->height <= 0) return;

   flushBatch();
   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   uint32_t offY, offU, offV;
   if (cellGcmAddressToOffset((void *)p->y, &offY) != CELL_OK || cellGcmAddressToOffset((void *)p->u, &offU) != CELL_OK ||
       cellGcmAddressToOffset((void *)p->v, &offV) != CELL_OK) return;

   // the picture's rectangle in the luma plane (a packed one: all of it)
   float u0 = (float)p->left / (float)p->planeW, u1 = (float)(p->left + p->width) / (float)p->planeW;
   float t0 = (float)p->top / (float)p->planeH,  t1 = (float)(p->top + p->height) / (float)p->planeH;

   // one opaque quad through the shared batch arrays
   float x0 = toClipX(x), y0 = toClipY(y), x1 = toClipX(x + w), y1 = toClipY(y + h);
   uint32_t white = 0xFFFFFFFFu;
   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ x0, y0, 0.0f, white, u0, t0 };
   v[1] = (GfxVertex){ x1, y0, 0.0f, white, u1, t0 };
   v[2] = (GfxVertex){ x0, y1, 0.0f, white, u0, t1 };
   v[3] = (GfxVertex){ x1, y0, 0.0f, white, u1, t0 };
   v[4] = (GfxVertex){ x1, y1, 0.0f, white, u1, t1 };
   v[5] = (GfxVertex){ x0, y1, 0.0f, white, u0, t1 };
   batchVertCount += VERTS_PER_RECT;
   // (V3.1.0 Test 4) shared: the picture's commands are one burst; one that cannot be written drops the picture
   if (!gfxBurstBegin()) { batchFlushStart = batchVertCount; return; }

   // the chroma sample point for a luma one: a luma sample at s (counted from the plane's edge, the picture
   // starting at p->left) is the chroma sample at (s - left) / 2 + leftC. Normalised:
   // uc = u * planeW / (2 planeWC) + (leftC - left / 2) / planeWC, the same for rows. A packed picture gives
   // (1, 1, 0, 0). Written into the ucode (an inline transfer in the command stream) only when it changes.
   float xf[4] = { (float)p->planeW / (2.0f * (float)p->planeWC), (float)p->planeH / (2.0f * (float)p->planeHC),
                   ((float)p->leftC - (float)p->left * 0.5f) / (float)p->planeWC,
                   ((float)p->topC - (float)p->top * 0.5f) / (float)p->planeHC };
   if (shaderYuvChromaXf && (!yuvChromaXfValid || memcmp(xf, yuvChromaXfSet, sizeof xf) != 0)) {
      cellGcmSetFragmentProgramParameter(CTX, shaderFpYuv, shaderYuvChromaXf, xf, shaderFpYuvOffset);
      memcpy(yuvChromaXfSet, xf, sizeof xf);
      yuvChromaXfValid = 1;
   }

   cellGcmSetVertexProgram(CTX, shaderVp, shaderVpUcode);
   cellGcmSetFragmentProgram(CTX, shaderFpYuv, shaderFpYuvOffset);

   uint32_t startByte = batchFlushStart * sizeof(GfxVertex);
   cellGcmSetVertexDataArray(CTX, shaderPosIdx, 0, sizeof(GfxVertex), 3, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderColIdx, 0, sizeof(GfxVertex), 4, CELL_GCM_VERTEX_UB, CELL_GCM_LOCATION_LOCAL, batchColOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderUvIdx, 0, sizeof(GfxVertex), 2, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchUvOffset[backBuffer] + startByte);

   setYuvPlaneTexture(shaderYuvTexUnit[0], offY, p->planeW, p->planeH, p->pitchY);
   setYuvPlaneTexture(shaderYuvTexUnit[1], offU, p->planeWC, p->planeHC, p->pitchC);
   setYuvPlaneTexture(shaderYuvTexUnit[2], offV, p->planeWC, p->planeHC, p->pitchC);

   cellGcmSetInvalidateTextureCache(CTX, CELL_GCM_INVALIDATE_TEXTURE);
   cellGcmSetCullFaceEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetBlendEnable(CTX, CELL_GCM_FALSE);
   if (gfxShareActive) cellGcmSetInvalidateVertexCache(CTX);   // (V3.1.0 Test 4, as in flushBatch)
   cellGcmSetDrawArrays(CTX, CELL_GCM_PRIMITIVE_TRIANGLES, 0, 6);
   if (!gfxBurstEnd(2)) yuvChromaXfValid = 0;   // (taken back: the chroma parameter never reached the RSX)

   batchFlushStart = batchVertCount;
}

void setGfxVsync(GfxVsync vsync)
{
   cellGcmSetFlipMode(vsync == GFX_VSYNC_ON ? CELL_GCM_DISPLAY_VSYNC : CELL_GCM_DISPLAY_HSYNC);
}

void endGfxFrame(void)
{
   if (!initialized) return;

   flushBatch();
   reportFrameBudget();
   if (gfxShareActive) { endGfxFrameShared(); return; }   // (V3.1.0 Test 4) no cellGcmSetWaitFlip there (rule 4)

   // queue this frame's flip without blocking the CPU on it. cellGcmSetWaitFlip is a gpu-side barrier:
   // the RSX won't draw into a buffer still on screen, so no tearing beyond what vsync-off already implies.
   if (cellGcmSetFlip(CTX, backBuffer) != CELL_OK) return;
   cellGcmFlush(CTX);
   cellGcmSetWaitFlip(CTX);
   flipsSubmitted++;

   backBuffer = (backBuffer + 1) % FRAME_COUNT;

   // stall only when the pipeline is full - one buffer on screen plus FRAME_COUNT-2 flips already queued.
   // with three buffers the CPU may run one flip ahead, so a slow GPU frame overlaps the next frame's work
   // instead of stalling us. the timeout is a safety net: a missed flip callback falls back to a brief wait
   // rather than a hard-lock.
   uint64_t startUs = sys_time_get_system_time();
   while ((uint32_t)(flipsSubmitted - flipsCompleted) >= FRAME_COUNT - 1) {
      if (sys_time_get_system_time() - startUs > 50000) break;   // 50ms guard, never a hard-lock
      sys_timer_usleep(200);
   }
   lastFlipWaitUs = sys_time_get_system_time() - startUs;
}

void finishGfx(void)
{
   if (!initialized) return;
   static uint32_t finishRef = 0;
   flushBatch();              // emit any pending quads first
   // (V3.1.0 Test 4) shared: a drain of at most 2 s on label 255 - cellGcmFinish waits without a bound, and outside the lock
   if (gfxShareActive) {
      if (gfxDrain(DRAIN_US) != 0) gfxBrake("the RSX did not finish its commands within 2 s (finishGfx)");
      gfxLogBrake();
      return;
   }
   cellGcmFinish(CTX, ++finishRef);  // block until the RSX drains the command buffer
}

// virtual canvas size, not the TV's real resolution -- all layout happens in this space
int getGfxScreenWidth(void)  { return VIRTUAL_W; }
int getGfxScreenHeight(void) { return VIRTUAL_H; }

int getGfxCoreClockMhz(void)
{
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   return (int)(config.coreFrequency / 1000000);
}

int getGfxMemoryClockMhz(void)
{
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   return (int)(config.memoryFrequency / 1000000);
}

size_t getUsedVram(void)
{
   return vramAllocated;
}

size_t getFreeVram(void)
{
   size_t total = 0;
   for (VramBlock *block = blockListHead; block; block = block->next)
      if (block->isFree) total += block->size;
   return total;
}

size_t getLargestFreeBlock(void)
{
   size_t largest = 0;
   for (VramBlock *block = blockListHead; block; block = block->next)
      if (block->isFree && block->size > largest) largest = block->size;
   return largest;
}

void freeGfxTexture(GfxTexture *tex)
{
   // offset 0 is frame buffer 0, never a texture, so it doubles as "empty".
   if (!tex || tex->offset == 0) return;
   freeVram((uint8_t *)vramBase + tex->offset);
   memset(tex, 0, sizeof(*tex));
}

// Hand a texture back without waiting for the GPU. The frame being drawn may still name it, so it
// is only released once that frame has been shown, which is what the next beginGfxFrame means.
void retireGfxTexture(GfxTexture *tex)
{
   if (!tex || tex->offset == 0) return;

   if (gfxShareActive) {
      // (V3.1.0 Test 4) shared: without a wait for the flip in the command stream the next frame does not mean the RSX is
      // through with it - the flip of the frame drawing now does. A full list waits for the RSX (finishGfx) and frees all
      if (retiredCount >= GFX_RETIRED_MAX) {
         finishGfx();
         for (int index = 0; index < retiredCount; index++) freeVram((uint8_t *)vramBase + retired[index]);
         retiredCount = 0;
         freeGfxTexture(tex);
         return;
      }
      retired[retiredCount] = tex->offset;
      retiredFlip[retiredCount] = getGfxNextFlipId();
      retiredCount++;
      memset(tex, 0, sizeof(*tex));
      return;
   }

   if (retiredCount < GFX_RETIRED_MAX) retired[retiredCount++] = tex->offset;
   else freeGfxTexture(tex);   // the list is full, so this one waits for the GPU the old way

   memset(tex, 0, sizeof(*tex));
}

static void freeRetiredTextures(void)
{
   if (gfxShareActive) {   // (V3.1.0 Test 4) the ones whose flip has happened
      uint32_t done = flipsCompleted;
      int kept = 0;
      for (int index = 0; index < retiredCount; index++) {
         if ((int32_t)(done - retiredFlip[index]) >= 0) {
            freeVram((uint8_t *)vramBase + retired[index]);
         } else {
            retired[kept] = retired[index];
            retiredFlip[kept] = retiredFlip[index];
            kept++;
         }
      }
      retiredCount = kept;
      return;
   }
   for (int index = 0; index < retiredCount; index++) freeVram((uint8_t *)vramBase + retired[index]);
   retiredCount = 0;
}

// loadGfxTexture now lives in image-loader.c (generic PNG/JPEG); see image-loader.h.

// ---- offscreen render targets (render-to-texture) ----
// A target is a pitch-linear A8R8G8B8 surface in VRAM -- the SAME layout as a normal
// texture, so after rendering we can sample rt.tex directly through the usual draw path.

int createGfxRenderTarget(GfxRenderTarget *rt, int w, int h)
{
   if (!initialized || !rt || w <= 0 || h <= 0 || w > MAX_TEX_DIM || h > MAX_TEX_DIM) return -1;
   uint32_t pitch = ((uint32_t)(w * 4) + 63) & ~63;   // RSX color-surface pitch: 64B multiple
   uint32_t size  = pitch * (uint32_t)h;
   void *pixels = allocateVram(size, 128);            // 128B align (>= color-surface req)
   if (!pixels) { memset(rt, 0, sizeof(*rt)); return -1; }

   uint32_t offset;
   cellGcmAddressToOffset(pixels, &offset);

   memset(rt, 0, sizeof(*rt));
   rt->tex.offset = offset;
   rt->tex.w = w;
   rt->tex.h = h;
   rt->tex.pitch = (int)pitch;
   return 0;
}

void beginGfxRenderTarget(GfxRenderTarget *rt)
{
   if (!initialized || !rt || rt->tex.offset == 0) return;
   flushBatch();   // anything still queued belongs to the previous target
   setSurface(rt->tex.offset, (uint32_t)rt->tex.pitch, rt->tex.w, rt->tex.h, rt->tex.w, rt->tex.h);
   targetIsDisplay = 0;
}

// Returns a CPU-readable pointer to the FRONT (currently displayed) framebuffer in local memory --
// the last fully-rendered+flipped frame -- for screenshots. Writes the screen width/height and the
// row pitch in bytes; pixels are A8R8G8B8. Valid until the next flip; NULL if gfx isn't initialised.
// (On RPCS3 this needs the "Write Color Buffers" GPU option so rendered buffers are mirrored back to
// emulated memory; on real PS3 local memory is readable directly, like the debug-bridge does.)
const void *getGfxDisplayBuffer(int *w, int *h, int *pitch)
{
   if (!initialized) return (const void *)0;
   finishGfx();   // ensure the RSX has finished the frame that produced the front buffer
   uint32_t front = (backBuffer + FRAME_COUNT - 1) % FRAME_COUNT;   // the buffer flipped last
   if (w)     *w = screenW;
   if (h)     *h = screenH;
   if (pitch) *pitch = (int)framePitch;
   return (const uint8_t *)vramBase + frameOffset[front];
}

void endGfxRenderTarget(void)
{
   if (!initialized) return;
   flushBatch();        // commit the target's draws before we rebind the display buffer
   setRenderTarget();   // back to the display buffer + screen viewport/scissor
   // flushBatch() already issues SetInvalidateTextureCache before the next draw, so a
   // subsequent sample of this freshly rendered target re-reads it (RSX is in-order, so
   // the surface writes precede that read). Verify on HW when wiring transitions.
}

void freeGfxRenderTarget(GfxRenderTarget *rt)
{
   if (!rt) return;
   freeGfxTexture(&rt->tex);   // frees the VRAM and zeroes tex
}

uint32_t uploadGfxTexture(const void *rgba, int w, int h, int srcPitch)
{
   if (w <= 0 || h <= 0 || w > MAX_TEX_DIM || h > MAX_TEX_DIM) return 0;
   uint32_t alignedPitch = ((uint32_t)(w * 4) + 63) & ~63;
   uint32_t size = alignedPitch * (uint32_t)h;
   void *pixels = allocateVram(size, 64);
   if (!pixels) return 0;

   uint8_t *dst = (uint8_t *)pixels;
   const uint8_t *src = (const uint8_t *)rgba;
   for (int row = 0; row < h; ++row) {
      memcpy(dst + row * alignedPitch, src + row * srcPitch, (uint32_t)(w * 4));
   }

   uint32_t offset;
   cellGcmAddressToOffset(pixels, &offset);
   return offset;
}

void updateGfxTexture(uint32_t offset, const void *rgba, int w, int h, int srcPitch, int slotW, int slotH)
{
   uint32_t alignedPitch = ((uint32_t)(slotW * 4) + 63) & ~63;
   uint8_t *dst = (uint8_t *)vramBase + offset;
   const uint8_t *src = (const uint8_t *)rgba;
   uint32_t rowBytes = (uint32_t)(w * 4);

   // copy content rows, clearing any right-margin padding
   for (int row = 0; row < h; ++row) {
      uint8_t *rowDst = dst + row * alignedPitch;
      memcpy(rowDst, src + row * srcPitch, rowBytes);
      if (rowBytes < alignedPitch)
         memset(rowDst + rowBytes, 0, alignedPitch - rowBytes);
   }

   // clear any bottom rows beyond the new content
   if (h < slotH) {
      memset(dst + h * alignedPitch, 0, alignedPitch * (uint32_t)(slotH - h));
   }
}

// ============================================================================
// (V3.1.0 Test 4) the shared RSX for the player (gfx.h) and for TEE VP9's RSX part (tv9_rsx_host.h)
// ============================================================================

void setGfxShareMode(int on, const char *test)
{
   if (initialized) return;   // (before initGfx: it decides how the command buffer is set up)
   gfxShareWanted = on ? 1 : 0;
   gfxSmallCb = test && strcmp(test, "small-cb") == 0;
   gfxNoContextTest = test && strcmp(test, "fail-start") == 0;
}

int isGfxShared(void) { return gfxShareActive; }

int isGfxRsxAllowed(void)
{
   return gfxShareActive && !rsxBrake && !rsxCrashOff && localPool != NULL;
}

int isGfxRsxReadable(const void *address)
{
   uint32_t offset;
   return address && cellGcmAddressToOffset(address, &offset) == CELL_OK;
}

int pauseGfxDrawing(uint32_t timeoutUs)
{
   if (!gfxShareActive) return 0;
   if (gfxThread() != uiThread) {
      (void)__sync_fetch_and_add(&pauseCount, 1);
      pauseSinceUs = gfxNowUs();
      __sync_synchronize();
      // a burst of the UI's under way is through after this; every later one sees the pause under the lock (gfxBurstBegin)
      if (lockForDrain(timeoutUs)) unlockCmd();
   }
   if (gfxDrain(timeoutUs) == 0) return 0;
   gfxBrake("the RSX did not finish its commands before a TEE VP9 decoder closed");
   gfxLogBrake();
   return -1;
}

void resumeGfxDrawing(void)
{
   if (!gfxShareActive || gfxThread() == uiThread) return;
   for (;;) {   // (endGfxFrameShared may have ended the pause already, after PAUSE_MAX_US)
      int count = pauseCount;
      if (count <= 0 || __sync_bool_compare_and_swap(&pauseCount, count, count - 1)) return;
   }
}

void brakeGfxRsx(const char *why)
{
   if (!gfxShareActive) return;
   gfxBrake(why);
   gfxLogBrake();   // (the player calls it outside every lock)
}

// the crash marker: written when a decoder's RSX part predicts (and pushed to the disk - a hang leaves no time), deleted at
// its close. A start that finds it runs without the RSX part once (setUpSharedBuffer)
void setGfxRsxActive(int on)
{
   if (!gfxShareActive) return;
   if (!on) { deleteFile(RSX_MARKER); return; }
   if (writeFile(RSX_MARKER, "1\n", 2) != 0 && makeDir(DBG_DIR) == 0) writeFile(RSX_MARKER, "1\n", 2);
   syncPath(RSX_MARKER);
}

// ---- tv9_rsx_host.h. All nine are inert while the buffer is not shared: no context, no room, no restart, no RSX memory, so
// the block's set-up says "no context" and its stream plays on the SPEs alone (the player does not ask for the RSX then) ----

void *tv9h_context(void)
{
   return gfxShareActive && !rsxBrake && !rsxCrashOff && !gfxNoContextTest ? (void *)CTX : NULL;
}

// rule 1 for the block: in slices of 2 s until it has the lock - it cannot take a failure. Its rows count as a change of state
void tv9h_lock(void)
{
   if (!cmdLockOk) return;
   for (;;) {
      int rc = sys_mutex_lock(cmdLock, BLOCK_LOCK_SLICE_US);
      if (rc == CELL_OK) break;
      if (rc != ETIMEDOUT) {
         if (!lockError) lockError = rc;
         sys_timer_usleep(1000);   // (an error that stays: no spin at the block's priority)
      }
   }
   blockGen++;
   blockLockedAtUs = gfxNowUs();
}

void tv9h_unlock(void)
{
   if (!cmdLockOk) return;
   uint32_t heldUs = (uint32_t)(gfxNowUs() - blockLockedAtUs);
   if (heldUs > shared.blockHoldMaxUs) shared.blockHoldMaxUs = heldUs;
   sys_mutex_unlock(cmdLock);
}

int tv9h_cb_room(void)
{
   if (!gfxShareActive || rsxBrake) return 0;   // (parked: end = current, 0 as well)
   return (int)((uint8_t *)CTX->end - (uint8_t *)CTX->current);
}

int tv9h_cb_restart(int timeout_us)
{
   if (!gfxShareActive || rsxBrake) return -1;
   if (gfxCbRestart(timeout_us > 0 ? (uint32_t)timeout_us : 0, 1) == 0) return 0;
   gfxBrake("the RSX did not read the command buffer to its end within the block's 2 s");
   return -1;
}

unsigned tv9h_state_gen(void)
{
   return stateGen;
}

// the block starts (1) or ends (0) predicting. The app keeps rule 4, so the start asks nothing of it. The end comes after the
// block's own waits and before it unmaps its memory (rsx_mc_stop, and rsx_mc_start's failures): everything the block and the
// app wrote until then is drained, bounded, the pause or no pause - an RSX that reads unmapped memory hangs
void tv9h_predicting(int on)
{
   if (!gfxShareActive || on) return;
   if (gfxDrain(DRAIN_US) != 0) gfxBrake("the RSX did not finish its commands within 2 s after TEE VP9's RSX part stopped");
   gfxLogBrake();   // (the block calls this outside the lock)
}

// RSX memory for the block from the pool: one piece after the other, given back together - rsx_mc_release frees all 13 it
// takes at once, so the pool starts over when nothing is out
void *tv9h_local_alloc(uint32_t align, uint32_t bytes)
{
   if (!gfxShareActive || !localPool || (align & (align - 1))) return NULL;
   if (!align) align = 1;
   void *block = NULL;
   sys_lwmutex_lock(&localLock, 0);
   uintptr_t at = ((uintptr_t)localPool + localUsed + align - 1) & ~(uintptr_t)(align - 1);
   uint32_t used = (uint32_t)(at - (uintptr_t)localPool);
   int slot = -1;
   for (int i = 0; i < LOCAL_BLOCKS && slot < 0; i++) if (!localBlocks[i]) slot = i;
   if (slot >= 0 && used <= LOCAL_POOL_BYTES && bytes <= LOCAL_POOL_BYTES - used) {
      block = (void *)at;
      localBlocks[slot] = block;
      localUsed = used + bytes;
      localLive++;
   }
   sys_lwmutex_unlock(&localLock);
   return block;
}

void tv9h_local_free(void *p)
{
   if (!p || !localPool) return;
   sys_lwmutex_lock(&localLock, 0);
   for (int i = 0; i < LOCAL_BLOCKS; i++)
      if (localBlocks[i] == p) {
         localBlocks[i] = NULL;
         if (--localLive == 0) localUsed = 0;
         break;
      }
   sys_lwmutex_unlock(&localLock);
}

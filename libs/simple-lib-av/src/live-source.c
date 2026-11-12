// live-source - continuous fragmented-mp4 stream over a DASH live sq/<n> segment sequence (see live-source.h).
//
// A background prefetch thread fetches segments ahead into a rolling window, so the demuxer/decoder reading
// the window never blocks on a segment download - without it, playback stalls once per segment and then
// bursts to catch up. One lock guards the window (producer = prefetch, consumer = the reading decode thread).
//
// A segment always goes into the window whole (V1.1 Test10). The window used to be a fixed 8 MB that cut a
// segment short when it did not fit, and 1080p60 live has segments of up to 4 MB a 2 s segment and up to 10 MB a
// 5 s one (measured 2026-11-07): the demuxer then read the next segment's bytes as samples and a box size out of
// the middle of an mdat, and waited for good at an offset that never comes - the picture stopped while the sound
// played on (TEE's console, Test9). Now each segment is fetched straight into the window's free end (no second
// buffer), a video stream's window is sized at the open for the kept history, the segment being read and a whole
// one ahead (no growing it - and no copy of it - while the TEE Decoder holds 91 MB), and the prefetch fetches the
// next segment once there is room for it, or at once when the reader has begun the last one.

#include "live-source.h"
#include "http.h"
#include "string-utilities.h"   // strCopy
#include "thread.h"             // lock + thread helpers
#include "dbg.h"

#include <sys/memory.h>          // (Test 16) sys_memory_allocate: the window outside the malloc heap
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WINDOW_VIDEO       (24 * 1024 * 1024)   // a video stream's window: history, the segment read, one ahead
                                                // (1 + 10 + 10 MB for 5 s segments of 1080p60)
#define WINDOW_AUDIO       (2 * 1024 * 1024)    // an audio stream's: its segments are ~80 KB
#define WINDOW_MAX         (40 * 1024 * 1024)   // a segment bigger than any before may grow the window up to this
#define WINDOW_BACKLOG     (1024 * 1024)        // history kept before the read position (the demuxer re-reads a box)
#define FETCH_MARGIN       (64 * 1024)          // room beyond a segment, so a full fetch is told from a cut one
#define TARGET_AHEAD       (4 * 1024 * 1024)    // how far ahead of the reader the prefetch keeps the window filled...
#define MAX_SEGMENTS_AHEAD 3                    // ...in at most this many whole segments the reader has not begun
                                                // (audio: 80 KB a segment, where 4 MB would be minutes of it)
#define SEGMENT_RING       16                   // where the latest segments begin, to count those ahead
#define START_BEHIND_EDGE  3                    // begin this many segments back from the edge for a small buffer
#define EDGE_POLL_MS       250                  // wait between retries for the next segment at the live edge
#define RECONNECT_MS       200                  // backoff after a failed segment fetch
#define PREFETCH_IDLE_MS   8                    // buffered far enough ahead: brief idle
#define READ_WAIT_MS       2                    // reader has nothing buffered yet: let the prefetch catch up

// googlevideo segment urls are signed, so any user-agent is accepted.
static const char *SEGMENT_UA = "Mozilla/5.0 (PLAYSTATION 3; 4.9)";

struct LiveSource {
   char     base[2048];   // "<...>/" - append "sq/<n>"
   long     nextSq;       // next segment number to fetch (prefetch-thread owned)
   long     edgeSq;       // the live edge the source was opened with (an ended stream's last segment)
   int      ended;        // a stream that is over: edgeSq is the end
   volatile int finished; // ...and its last segment is in: a read past it is the end of the stream
   volatile long firstSq; // the segment the stream began with, -1 until it is in
   int      firstDone;    // the first (init-bearing) segment has been emitted
   int      failStreak;   // consecutive failed fetches of the current segment, for log throttling

   uint8_t *window;       // rolling buffer of the virtual stream (guarded by lock; only the prefetch moves or grows it)
   int      windowCap;    // its size: WINDOW_VIDEO or WINDOW_AUDIO, up to WINDOW_MAX
   int      windowKernel;   // (Test 16) the window is the kernel's (allocWindow), not the heap's
   int      largest;      // the biggest segment so far (prefetch-thread owned)
   int      windowLen;
   uint64_t windowStart;  // virtual offset of window[0]
   uint64_t position;     // current read position (virtual)
   uint64_t segStart[SEGMENT_RING];   // virtual offsets the latest segments begin at...
   int      segCount;     // ...and how many segments went in

   sys_lwmutex_t    lock;
   int              lockReady;
   sys_ppu_thread_t prefetchThread;
   int              threadActive;
   volatile int     running;
   volatile int     cancel;
};

// offset of the first 'moof' box (skipping a segment's leading ftyp/styp/sidx/moov/emsg). 0 if none found.
static int firstMoofOffset(const uint8_t *data, int length)
{
   uint32_t pos = 0;
   while (pos + 8 <= (uint32_t)length) {
      uint32_t size = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
      if (data[pos + 4] == 'm' && data[pos + 5] == 'o' && data[pos + 6] == 'o' && data[pos + 7] == 'f') return (int)pos;
      if (size < 8) break;
      pos += size;
   }
   return 0;
}

// drops the stream already read from the window's front, keeping WINDOW_BACKLOG before the read position (all of
// it when the reader is past the window's end). Caller holds the lock.
static void dropHistory(LiveSource *source)
{
   uint64_t keepFrom = source->position > source->windowStart + WINDOW_BACKLOG ? source->position - WINDOW_BACKLOG
                                                                               : source->windowStart;
   uint64_t drop = keepFrom - source->windowStart;
   if (drop > (uint64_t)source->windowLen) drop = (uint64_t)source->windowLen;
   if (drop == 0) return;
   memmove(source->window, source->window + drop, (size_t)((uint64_t)source->windowLen - drop));
   source->windowLen -= (int)drop;
   source->windowStart += drop;
}

// (Test 16) The window comes from the kernel in whole MB, not from the malloc heap, and goes back to it on close.
// The heap keeps what a closed decoder gave back (the TEE Decoder's 85 MB at 1080p) and never returns it; a live
// stream's 26 MB taken out of that hole left the next TEE Decoder's 32 MB block no room, the heap grew into the last
// of the kernel's memory, the SPEs could not start (0x80010004) and cellVdec could not open either (0x80610180):
// "couldn't open stream" on every live stream after a 1080p video, on TEE's console. Should the kernel have no
// whole MB left, the heap's memory is taken as before. bytes is a whole number of MB.
static uint8_t *allocWindow(size_t bytes, int *kernel)
{
   sys_addr_t address = 0;
   if (sys_memory_allocate(bytes, SYS_MEMORY_PAGE_SIZE_1M, &address) == CELL_OK && address) {
      *kernel = 1;
      return (uint8_t *)(uintptr_t)address;
   }
   *kernel = 0;
   return (uint8_t *)malloc(bytes);
}

static void freeWindow(uint8_t *window, int kernel)
{
   if (!window) return;
   if (kernel) sys_memory_free((sys_addr_t)(uintptr_t)window);
   else        free(window);
}

// makes the window hold at least `need` bytes (in whole MB, up to WINDOW_MAX) - for a segment bigger than any
// before, which the open's size did not plan for. Under the lock, as the reader copies out of it. 1 ok, 0 too big
// or out of memory.
static int growWindow(LiveSource *source, uint64_t need)
{
   if (need <= (uint64_t)source->windowCap) return 1;
   if (need > WINDOW_MAX) return 0;
   uint64_t want = (need + 0xFFFFF) & ~(uint64_t)0xFFFFF;
   lock(&source->lock);
   int kernel = 0;
   uint8_t *bigger = allocWindow((size_t)want, &kernel);
   if (bigger) {
      memcpy(bigger, source->window, (size_t)source->windowLen);
      freeWindow(source->window, source->windowKernel);
      source->window = bigger;
      source->windowKernel = kernel;
      source->windowCap = (int)want;
   }
   unlock(&source->lock);
   if (bigger) logInfo("[live] window now %d MB\n", (int)(want >> 20));
   return bigger != NULL;
}

// whole segments in the window that the reader has not begun (caller holds the lock)
static int segmentsAhead(const LiveSource *source)
{
   int ahead = 0, kept = source->segCount < SEGMENT_RING ? source->segCount : SEGMENT_RING;
   for (int i = 0; i < kept; i++)
      if (source->segStart[i] >= source->position) ahead++;
   return ahead;
}

// fetch the next segment straight into the window's free end (network, no lock: the reader only reads below the
// window's end, and only this thread moves it), then strip its init and publish it (under lock). 0 ok, 1 edge,
// 2 an ended stream's last segment is in, -1 error.
static int fetchNextSegment(LiveSource *source)
{
   if (source->ended && source->nextSq > source->edgeSq) return 2;
   char url[2200];
   // A manifest-derived base takes a path segment ("<base>sq/<n>"). The X-Head-Seqnum fallback in
   // youtube.c (used when the client gets no dashManifestUrl) hands over a query-style base that
   // already ends in "&sq=", where the number is appended directly instead.
   size_t baseLen = strlen(source->base);
   if (baseLen > 0 && source->base[baseLen - 1] == '=')
      snprintf(url, sizeof url, "%s%ld", source->base, source->nextSq);
   else
      snprintf(url, sizeof url, "%ssq/%ld", source->base, source->nextSq);
   HttpHeader headers[] = { { "User-Agent", SEGMENT_UA }, { "Accept-Encoding", "identity" } };
   char wholeText[24];
   HttpHeaderCapture whole = { "content-length", wholeText, sizeof wholeText };   // lowercase: see youtube.c

   int length = 0, status = 0, rc;
   uint8_t *at;
   for (;;) {
      // room at the window's end; the read history goes first when that end is short of the biggest segment yet
      // (of half the window before the first)
      lock(&source->lock);
      int wanted = source->largest > 0 ? source->largest + FETCH_MARGIN : source->windowCap / 2;
      if (source->windowCap - source->windowLen < wanted) dropHistory(source);
      at = source->window + source->windowLen;
      int room = source->windowCap - source->windowLen;
      unlock(&source->lock);

      rc = fetchHttpCapturing("GET", url, headers, 2, NULL, 0, (char *)at, room, &length, &status, &whole);
      // fetchHttp stops at room - 1 without saying the body went on: a segment that did not fit is fetched
      // again into a window grown to its Content-Length (or twice what came where the reply has none)
      if (rc != 0 || (status != 200 && status != 206) || length < room - 1) break;
      uint64_t body = (uint64_t)atol(wholeText);
      uint64_t need = (uint64_t)source->windowLen + (body > (uint64_t)length ? body : 2 * (uint64_t)length) + FETCH_MARGIN;
      if (!growWindow(source, need)) {
         logError("[live] sq/%ld does not fit the window (%d KB fetched, %d KB in all): skipped\n", source->nextSq,
                  length / 1024, (int)(body / 1024));
         source->nextSq++;
         return -1;
      }
   }
   if (rc == 0 && (status == 204 || status == 404)) {
      // 204: not generated yet, we're at the live edge (googlevideo holds the request ~7.5 s first). A 404 behind
      // the edge is a segment the server no longer has - a rewind past its window (answered after ~8 s): before
      // anything of the stream went out, it begins at the live edge instead. Otherwise wait for the edge.
      if (status == 404 && !source->ended && !source->firstDone && source->nextSq < source->edgeSq - START_BEHIND_EDGE) {
         logWarn("[live] sq/%ld is gone (404): beginning at the live edge, sq/%ld\n", source->nextSq,
                 source->edgeSq - START_BEHIND_EDGE);
         source->nextSq = source->edgeSq - START_BEHIND_EDGE;
         return 0;   // fetch that one at once
      }
      return 1;
   }
   if (rc != 0 || length <= 0 || (status != 200 && status != 206)) {
      // the caller retries the same segment every 200ms, so a network outage would log 5 lines a second
      // forever; log the first failure and then one per ~5s of outage
      if (source->failStreak++ % 25 == 0)
         logError("[live] segment sq/%ld failed rc=%d status=%d (fail #%d)\n", source->nextSq, rc, status, source->failStreak);
      return -1;
   }
   source->failStreak = 0;
   if (length > source->largest) source->largest = length;

   // every segment after the stream's first comes with the init again: from its first moof on
   int offset = source->firstDone ? firstMoofOffset(at, length) : 0;
   lock(&source->lock);
   if (offset > 0) memmove(at, at + offset, (size_t)(length - offset));
   source->segStart[source->segCount % SEGMENT_RING] = source->windowStart + (uint64_t)source->windowLen;
   source->segCount++;
   source->windowLen += length - offset;
   if (!source->firstDone) source->firstSq = source->nextSq;
   source->firstDone = 1;
   unlock(&source->lock);
   source->nextSq++;
   return 0;
}

static void prefetchThread(uint64_t arg)
{
   LiveSource *source = (LiveSource *)(uintptr_t)arg;
   while (source->running && !source->cancel) {
      lock(&source->lock);
      uint64_t end = source->windowStart + (uint64_t)source->windowLen;
      uint64_t ahead = end > source->position ? end - source->position : 0;
      int segments = segmentsAhead(source);
      // the room a fetch would have: the free end and the history it may drop
      uint64_t droppable = source->position > source->windowStart + WINDOW_BACKLOG
                         ? source->position - WINDOW_BACKLOG - source->windowStart : 0;
      if (droppable > (uint64_t)source->windowLen) droppable = (uint64_t)source->windowLen;
      uint64_t room = (uint64_t)(source->windowCap - source->windowLen) + droppable;
      int primed = source->firstDone;
      unlock(&source->lock);

      // Once the reader has begun the last segment in, the next one comes at once (a whole one ahead is what
      // carries 5 s segments of 10 MB). Otherwise up to TARGET_AHEAD in at most MAX_SEGMENTS_AHEAD segments - and
      // only when one as big as the biggest so far fits: the reader frees the room as it goes.
      int fetch = !primed || segments == 0 ||
                  (ahead < TARGET_AHEAD && segments < MAX_SEGMENTS_AHEAD &&
                   room >= (uint64_t)source->largest + FETCH_MARGIN);
      if (!fetch) { sleepMs(PREFETCH_IDLE_MS); continue; }

      int rc = fetchNextSegment(source);
      if (rc == 2) {                              // an ended stream is all in: the reader ends after it
         __sync_synchronize();
         source->finished = 1;
         logInfo("[live] the ended stream's last segment, sq/%ld, is in\n", source->edgeSq);
         break;
      }
      if (rc == 1) sleepMs(EDGE_POLL_MS);        // at the edge, wait then retry the same sq (it's live, more is coming)
      else if (rc < 0) sleepMs(RECONNECT_MS);    // transient failure: back off and retry the same sq
   }
   exitThread();
}

LiveSource *openLiveSource(const char *baseUrl, long startSq, long edgeSq, long playSq, int ended)
{
   LiveSource *source = (LiveSource *)calloc(1, sizeof *source);
   if (!source) return NULL;
   // googlevideo urls name the stream's type ("mime=audio%2Fmp4" as a parameter, ".../mime/audio%2Fmp4/..." in a
   // manifest's path): an audio stream's segments need a fraction of a video stream's window
   int audio = strstr(baseUrl, "audio%2F") != NULL || strstr(baseUrl, "audio/mp4") != NULL;
   source->windowCap = audio ? WINDOW_AUDIO : WINDOW_VIDEO;
   source->window = allocWindow((size_t)source->windowCap, &source->windowKernel);
   if (!source->window) { closeLiveSource(source); return NULL; }

   strCopy(source->base, sizeof source->base, baseUrl);
   long start = playSq >= 0 ? playSq : ended ? startSq : edgeSq - START_BEHIND_EDGE;
   if (ended && start > edgeSq) start = edgeSq;
   source->nextSq = start > startSq ? start : startSq;
   source->edgeSq = edgeSq;
   source->ended = ended;
   source->firstSq = -1;
   createLock(&source->lock);
   source->lockReady = 1;
   logInfo("[live] open %s sq %ld..%ld, starting at %ld%s\n", ended ? "ended stream" : "live", startSq, edgeSq,
           source->nextSq, playSq >= 0 ? " (asked for)" : "");

   source->running = 1;
   source->threadActive = spawnJoinableThread(&source->prefetchThread, prefetchThread, (uint64_t)(uintptr_t)source,
                          THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "live-fetch") == 0;
   if (!source->threadActive) { closeLiveSource(source); return NULL; }
   return source;
}

int64_t readLiveSource(LiveSource *source, void *buffer, uint64_t length)
{
   for (;;) {
      if (source->cancel) return 0;
      // read before the window is: finished goes up only after the last segment is in, so when it was up here,
      // an empty read position below is the end of the stream and not a segment still coming in
      int finished = source->finished;
      __sync_synchronize();
      lock(&source->lock);
      if (source->position < source->windowStart) { unlock(&source->lock); return -1; }   // seeked before dropped history
      uint64_t available = source->windowStart + source->windowLen > source->position ? source->windowStart + source->windowLen - source->position : 0;
      if (available > 0) {
         uint64_t take = length < available ? length : available;
         memcpy(buffer, source->window + (source->position - source->windowStart), take);
         source->position += take;
         unlock(&source->lock);
         return (int64_t)take;
      }
      unlock(&source->lock);
      if (finished) return 0;   // an ended stream read to its last byte
      sleepMs(READ_WAIT_MS);   // nothing buffered at the read position yet; let the prefetch catch up
   }
}

int seekLiveSource(LiveSource *source, uint64_t offset)
{
   lock(&source->lock);
   // not before the kept history; and not further past the window's end than a whole window - a demuxer that
   // jumps that far has read a box size out of the middle of the data, and waiting there would be for ever
   int ok = offset >= source->windowStart && offset <= source->windowStart + (uint64_t)source->windowLen + WINDOW_MAX;
   if (ok) source->position = offset;
   unlock(&source->lock);
   if (!ok && offset >= source->windowStart) logError("[live] a seek far past the stream's end refused\n");
   return ok ? 0 : -1;
}

uint64_t getLiveSourcePosition(const LiveSource *source) { return source->position; }

long getLiveSourceFirstSq(const LiveSource *source) { return source->firstSq >= 0 ? source->firstSq : source->nextSq; }

void cancelLiveSource(LiveSource *source) { if (source) source->cancel = 1; }

void closeLiveSource(LiveSource *source)
{
   if (!source) return;
   source->running = 0;
   source->cancel = 1;
   if (source->threadActive) { joinThread(source->prefetchThread); source->threadActive = 0; }
   if (source->lockReady) { destroyLock(&source->lock); source->lockReady = 0; }
   freeWindow(source->window, source->windowKernel);
   free(source);
}

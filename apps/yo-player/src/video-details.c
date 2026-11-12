// video-details - one resolve a video, kept for Home's list (see video-details.h).
//
// The resolve is the player's own (extractor->extract), and what it is read for is the player's own pick: the best
// of listPlayableVideos - the choices of the quality menu - and pickVideoForPlayback with the quality setting. The
// TEE Decoder counts as usable unless settings.txt says video-decoder=sony (or it failed earlier in this run), as
// the player has it. A resolve takes a second or two on the console; the thread writes only into `job`, which the
// UI thread copies into a slot once it is done.

#include "video-details.h"
#include "extractor.h"
#include "stream-select.h"      // listPlayableVideos / pickVideoForPlayback: what the player would play
#include "decode-h264.h"        // isH264TeeUsable
#include "settings.h"           // getQualityCap, getVideoDecoderSetting
#include "thread.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <sys/sys_time.h>
#include <sys/memory.h>     // sys_memory_get_user_memory_size: the log line says what is left
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DETAILS_SLOTS 64

static VideoDetails slots[DETAILS_SLOTS];
static int          slotCount;
static unsigned     useClock = 1;

static StreamInfo      *info;          // the resolve's (135 KB): allocated on the first one, kept
static VideoDetails     job;           // the thread's result
static int              jobTeeUsable, jobQualityCap;
static sys_ppu_thread_t jobTid;
static int              jobActive;
static volatile int     jobDone;
static uint64_t         jobStartUs;

static int shorterSide(const StreamFormat *format)
{
   if (format->width > 0 && format->height > 0) return format->width < format->height ? format->width : format->height;
   return format->height;
}

static void resolveThread(uint64_t arg)
{
   (void)arg;
   setExtractQuiet(1);   // the format lines of video after video would bury the log
   const Extractor *extractor = findExtractor(job.videoId);
   int ok = extractor && extractor->extract(job.videoId, info) == 0 && info->formatCount > 0;
   if (ok) {
      const StreamFormat *best[1];
      int count = listPlayableVideos(info, jobTeeUsable, best, 1);
      const StreamFormat *play = pickVideoForPlayback(info, jobTeeUsable, jobQualityCap);
      const StreamFormat *top = count > 0 ? best[0] : play;
      if (top)  { job.bestHeight = shorterSide(top);  job.bestFps = top->fps; }
      if (play) { job.playHeight = shorterSide(play); job.playFps = play->fps; }
      job.qualityCap = jobQualityCap;
      job.isLive = info->isLive;
      job.liveDvr = info->liveDvr;
      job.captions = info->captionCount;
      job.audioTracks = info->audioTrackCount;
      job.durationSeconds = info->durationSeconds;
      int length = (int)strlen(info->description);
      if (length >= DETAILS_DESCRIPTION_CAP) {   // cut at a character's start, never inside one
         length = DETAILS_DESCRIPTION_CAP - 1;
         while (length > 0 && ((unsigned char)info->description[length] & 0xC0) == 0x80) length--;
         job.descriptionCut = 1;
      }
      memcpy(job.description, info->description, (size_t)length);
      job.description[length] = 0;
      if ((int)strlen(info->description) >= (int)sizeof info->description - 1) job.descriptionCut = 1;
   }
   job.failed = !ok;
   setExtractQuiet(0);
   __sync_synchronize();
   jobDone = 1;
   exitThread();
}

static int findSlot(const char *videoId)
{
   for (int i = 0; i < slotCount; i++) if (strcmp(slots[i].videoId, videoId) == 0) return i;
   return -1;
}

const VideoDetails *findVideoDetails(const char *videoId)
{
   if (!videoId || !videoId[0]) return NULL;
   int slot = findSlot(videoId);
   if (slot < 0) return NULL;
   slots[slot].used = useClock++;
   return &slots[slot];
}

int isVideoDetailsPending(const char *videoId)
{
   return jobActive && videoId && strcmp(job.videoId, videoId) == 0;
}

static void keepJob(void)
{
   int slot = findSlot(job.videoId);
   if (slot < 0 && slotCount < DETAILS_SLOTS) slot = slotCount++;
   if (slot < 0) {   // full: the one looked at longest ago goes
      slot = 0;
      for (int i = 1; i < slotCount; i++) if (slots[i].used < slots[slot].used) slot = i;
   }
   slots[slot] = job;
   slots[slot].used = useClock++;
}

void updateVideoDetails(const char *const *wanted, int count)
{
   if (jobActive && jobDone) {
      joinThread(jobTid);
      jobActive = 0;
      char quality[16];
      formatVideoQuality(quality, sizeof quality, job.bestHeight, job.bestFps);
      sys_memory_info_t memory;
      unsigned freeMb = sys_memory_get_user_memory_size(&memory) == CELL_OK ? (unsigned)(memory.available_user_memory >> 20) : 0;
      logInfo("[details] %s: %s%s in %d ms, user memory %u MB free\n", job.videoId,
              job.failed ? "not resolved" : quality[0] ? quality : "nothing playable", job.isLive ? " (live)" : "",
              (int)((sys_time_get_system_time() - jobStartUs) / 1000), freeMb);
      keepJob();
   }
   if (jobActive) return;
   for (int i = 0; i < count; i++) {
      const char *videoId = wanted[i];
      if (!videoId || !videoId[0] || findSlot(videoId) >= 0) continue;
      if (!info) info = (StreamInfo *)malloc(sizeof *info);
      if (!info) return;
      memset(&job, 0, sizeof job);
      strCopy(job.videoId, sizeof job.videoId, videoId);
      jobTeeUsable = getVideoDecoderSetting() != VIDEO_DECODER_SONY && isH264TeeUsable();
      jobQualityCap = getQualityCap();
      jobDone = 0;
      jobStartUs = sys_time_get_system_time();
      // below the UI thread: the parse of a reply of a megabyte or two must not cost frames (and presses)
      jobActive = spawnJoinableThread(&jobTid, resolveThread, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                                      "yt-details") == 0;
      if (!jobActive) logWarn("[details] no thread for a resolve\n");
      return;
   }
}

void formatVideoQuality(char *out, int cap, int height, int fps)
{
   if (height <= 0) { if (cap > 0) out[0] = 0; return; }
   if (fps > 0) snprintf(out, cap, "%dp%d", height, fps);
   else         snprintf(out, cap, "%dp", height);
}

void stopVideoDetails(void)
{
   if (!jobActive) return;
   joinThread(jobTid);
   jobActive = 0;
   keepJob();   // done by now: as good as any other
}

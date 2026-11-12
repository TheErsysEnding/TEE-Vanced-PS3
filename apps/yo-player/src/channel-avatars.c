// channel-avatars - the channels' pictures as textures (see channel-avatars.h).
//
// One fetch at a time, on a joinable thread like the grid's hero picture: a picture is ~5 KB, so a page of them is in
// within a second or two, and one connection is all it ever takes beside the grid's own. The slots keep a decoded
// texture each (96 x 96, 36 KB of VRAM) - or the note that a channel's fetch failed, so it is not asked for again,
// or that no picture of it is known yet, looked at again a little later (a channel page or the account's list may
// bring one meanwhile).

#include "channel-avatars.h"
#include "extractor.h"          // getChannelAvatarUrl
#include "http.h"
#include "thread.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define AVATAR_TEXTURES  48
#define AVATAR_PX        96              // asked for at this size; drawn at 26 to 96
#define AVATAR_QUEUE     24
#define AVATAR_JPEG_CAP  (64 * 1024)
#define NO_URL_RETRY     180             // frames before a channel without a known picture is looked at again
#define AVATAR_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

typedef enum { SLOT_FREE, SLOT_READY, SLOT_FAILED, SLOT_NO_URL } SlotState;
typedef struct {
   char       id[32];
   SlotState  state;
   GfxTexture tex;
   unsigned   used;      // the frame it was last asked for
   unsigned   checked;   // SLOT_NO_URL: the frame its picture was last looked for
} AvatarSlot;

static AvatarSlot slots[AVATAR_TEXTURES];
static unsigned   frame = 1;
static char       queue[AVATAR_QUEUE][32];   // [0] the newest request
static int        queued;

static sys_ppu_thread_t fetchTid;
static int              fetchActive;
static volatile int     fetchDone, fetchOk;
static char             fetchId[32], fetchUrl[256];
static uint8_t         *fetchJpeg;
static int              fetchJpegLen;

static void fetchThread(uint64_t arg)
{
   (void)arg;
   uint8_t *buffer = (uint8_t *)malloc(AVATAR_JPEG_CAP);
   int ok = 0;
   if (buffer) {
      HttpHeader headers[] = { { "User-Agent", AVATAR_UA }, { "Accept-Encoding", "identity" } };
      int length = 0, status = 0;
      int rc = fetchHttp("GET", fetchUrl, headers, 2, NULL, 0, (char *)buffer, AVATAR_JPEG_CAP, &length, &status);
      if (rc == 0 && status == 200 && length > 0) { fetchJpeg = buffer; fetchJpegLen = length; ok = 1; }
      else free(buffer);
   }
   fetchOk = ok;
   __sync_synchronize();
   fetchDone = 1;
   exitThread();
}

static int findSlot(const char *id)
{
   for (int i = 0; i < AVATAR_TEXTURES; i++) if (slots[i].state != SLOT_FREE && strcmp(slots[i].id, id) == 0) return i;
   return -1;
}

// a free slot, else the one asked for longest ago - never one drawn this frame or the last (the RSX may still read
// it: retired, not freed, either way)
static int takeSlot(void)
{
   int best = -1;
   for (int i = 0; i < AVATAR_TEXTURES; i++) {
      if (slots[i].state == SLOT_FREE) return i;
      if (slots[i].used + 1 >= frame) continue;
      if (best < 0 || slots[i].used < slots[best].used) best = i;
   }
   if (best >= 0 && slots[best].tex.offset) retireGfxTexture(&slots[best].tex);
   if (best >= 0) memset(&slots[best], 0, sizeof slots[best]);
   return best;
}

static void enqueue(const char *id)
{
   if (fetchActive && strcmp(fetchId, id) == 0) return;
   for (int q = 0; q < queued; q++) {
      if (strcmp(queue[q], id) != 0) continue;
      if (q == 0) return;
      memmove(queue[1], queue[0], (size_t)q * sizeof queue[0]);   // asked again: to the front
      strCopy(queue[0], sizeof queue[0], id);
      return;
   }
   if (queued == AVATAR_QUEUE) queued--;   // full: the oldest request goes
   memmove(queue[1], queue[0], (size_t)queued * sizeof queue[0]);
   strCopy(queue[0], sizeof queue[0], id);
   queued++;
}

int hasChannelAvatar(const char *channelId)
{
   if (!channelId || strncmp(channelId, "UC", 2) != 0) return 0;
   int slot = findSlot(channelId);
   if (slot >= 0 && slots[slot].state == SLOT_READY) return 1;
   char url[256];
   return getChannelAvatarUrl(channelId, AVATAR_PX, url, sizeof url);
}

const GfxTexture *getChannelAvatarTexture(const char *channelId)
{
   if (!channelId || strncmp(channelId, "UC", 2) != 0) return NULL;
   int slot = findSlot(channelId);
   if (slot >= 0) {
      slots[slot].used = frame;
      if (slots[slot].state == SLOT_READY) return &slots[slot].tex;
      if (slots[slot].state != SLOT_NO_URL || frame - slots[slot].checked < NO_URL_RETRY) return NULL;
      slots[slot].checked = frame;   // looked for again (updateChannelAvatars)
   }
   enqueue(channelId);
   return NULL;
}

void updateChannelAvatars(void)
{
   frame++;
   if (fetchActive && fetchDone) {
      joinThread(fetchTid);
      fetchActive = 0;
      GfxTexture tex;
      memset(&tex, 0, sizeof tex);
      if (fetchOk && fetchJpeg) tex = loadGfxTextureMem(fetchJpeg, (uint32_t)fetchJpegLen);
      if (fetchJpeg) { free(fetchJpeg); fetchJpeg = NULL; }
      int slot = findSlot(fetchId);
      if (slot < 0) slot = takeSlot();
      if (slot >= 0) {
         if (slots[slot].tex.offset) retireGfxTexture(&slots[slot].tex);
         strCopy(slots[slot].id, sizeof slots[slot].id, fetchId);
         slots[slot].tex = tex;
         slots[slot].state = tex.offset ? SLOT_READY : SLOT_FAILED;
         slots[slot].used = frame;
      } else if (tex.offset) {
         freeGfxTexture(&tex);   // every slot on screen: asked for again once one comes free
      }
   }
   while (!fetchActive && queued > 0) {
      char id[32];
      strCopy(id, sizeof id, queue[0]);
      queued--;
      memmove(queue[0], queue[1], (size_t)queued * sizeof queue[0]);
      int slot = findSlot(id);
      if (slot >= 0 && slots[slot].state != SLOT_NO_URL) continue;   // in already, or failed
      if (!getChannelAvatarUrl(id, AVATAR_PX, fetchUrl, sizeof fetchUrl)) {
         if (slot < 0) slot = takeSlot();
         if (slot >= 0) {   // no picture known: looked at again in a while
            strCopy(slots[slot].id, sizeof slots[slot].id, id);
            slots[slot].state = SLOT_NO_URL;
            slots[slot].used = slots[slot].checked = frame;
         }
         continue;
      }
      strCopy(fetchId, sizeof fetchId, id);
      fetchDone = fetchOk = 0;
      fetchJpeg = NULL;
      fetchActive = spawnJoinableThread(&fetchTid, fetchThread, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                                        "yt-avatar") == 0;
      if (!fetchActive) logWarn("[avatars] no thread for a channel picture\n");
      break;
   }
}

void stopChannelAvatars(void)
{
   if (!fetchActive) return;
   joinThread(fetchTid);
   fetchActive = 0;
   if (fetchJpeg) { free(fetchJpeg); fetchJpeg = NULL; }
   enqueue(fetchId);   // asked for again on the way back
}

void termChannelAvatars(void)
{
   stopChannelAvatars();
   finishGfx();
   for (int i = 0; i < AVATAR_TEXTURES; i++) if (slots[i].tex.offset) freeGfxTexture(&slots[i].tex);
   memset(slots, 0, sizeof slots);
   queued = 0;
}

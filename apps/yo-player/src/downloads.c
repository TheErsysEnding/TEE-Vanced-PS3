// downloads - background queue that remuxes a video's streams into one .mp4 on disk (see downloads.h).

#include "downloads.h"
#include "stream-select.h"      // pickBestVideo / pickBestAudio
#include "mux-mp4.h"
#include "demux.h"              // VideoDemuxer + readVideoAu / readAudioAu
#include "video-source.h"       // (V3.0.0) the streams copied as they are, for the app's own downloads
#include "extractor.h"
#include "video-export.h"   // hand the finished file to the xmb

#include "vfs.h"                // renamePath/deleteFile + MAX_PATH_LEN
#include "thread.h"            // lwmutex + spawnJoinableThread
#include "string-utilities.h"   // strCopy
#include "font.h"
#include "ui/label.h"
#include "ui/icon-font.h"       // the chip's download arrow
#include "ui-fonts.h"           // the shared fonts and the label helpers: the overlay opens no font of its own
#include "ui-draw.h"            // safe area, pills, fixed-width digits
#include "ui-hints.h"           // UI_HINTS_BOTTOM_Y: by default the overlay sits just above the hint row
#include "theme.h"
#include "anim.h"               // interpolateColor: the chip's edge, blended once
#include "gfx.h"
#include "dbg.h"

#include "app-paths.h"          // getAppDataDir: the app's downloads live with its data
#include <sys/sys_time.h>       // sys_time_get_current_time: the date a download landed
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_QUEUE          16
#define DISPLAY_TITLE_MAX  32       // longest title shown in the "Downloading ..." overlay
// The overlay is an Ember chip (Bild 6's filter chips): a pill in surface-2 with the 2-px hairline inside
// its edge that the search field has, so it holds its own against the page behind it. It carries a title, so
// its text is reading text (20/600), not badge text (18/800). The percent counts up while it is on screen and
// is drawn in fixed-width digits, in the accent the app uses for progress: Manrope's own digits are
// proportional, and the chip's left edge would twitch at every step.
#define OVERLAY_TEXT_SIZE  UI_META_SIZE
#define OVERLAY_WEIGHT     UI_META_WEIGHT
#define PERCENT_WEIGHT     UI_WEIGHT_BOLD
#define OVERLAY_H          DOWNLOAD_OVERLAY_H
#define OVERLAY_EDGE       2
#define OVERLAY_PAD_L      16       // the pill's round end -> the arrow
#define OVERLAY_PAD_R      20       // the last text -> the pill's round end
#define OVERLAY_ICON       22
#define OVERLAY_ICON_GAP   10       // the arrow -> the text
#define OVERLAY_GAP        20       // between the overlay and the hint row under it
#define OVERLAY_TEXT_MAX_W 560      // a guard: 32 bytes of title come to about 500 px with the words round it

typedef struct {
   char videoId[16]; char title[160]; char author[48]; char duration[12]; int quality;
   int  fromGallery;     // (V3.0.0) an XMB download that replaces the app's own copy: deleted once the XMB has this
   int  remuxLocal;      // ... and is made from that copy's files, without YouTube
} QueueItem;

static struct {
   sys_lwmutex_t    mutex;         // guards queue[], count, workerSpawned
   QueueItem        queue[MAX_QUEUE];
   int              count;
   int              workerSpawned;
   sys_ppu_thread_t worker;

   volatile int     cancelAll;     // app exit: abort the current download and drain the queue
   volatile int     active;        // a download is currently running (drives the overlay)
   volatile int     percent;
   char             activeTitle[160];
   char             activeId[16];  // videoId of the running download (for dedupe; valid while active)
   volatile unsigned revision;     // (V3.0.0) the Downloads page's: a download landed or was deleted
   int              batchTotal;    // (V3.0.0) downloads asked for since the queue last ran empty ...
   int              batchDone;     // ... and how many of them are finished (or failed): the rail's "1/10"

   int              initialized;
} dl;

static Label    overlayLabel;      // `Downloading "<title>"`
static Label    queueLabel;        // "(+N)" while more downloads wait behind this one, else empty
static UiDigits percentDigits;     // the percent, in fixed-width digits
static char     percentText[8];    // " NN%" - the space before the number is part of it
static Icon     overlayIcon;       // the download arrow at the chip's left end
static uint32_t overlayEdge;       // the hairline over surface-2, opaque: the chip's 2-px edge

// ---- filename ----

// the video id, which is always plain characters: the export refuses filenames with spaces or brackets,
// and the name the XMB ends up showing comes from the title handed to the export, not from this one.
static void buildDownloadPath(char *path, int cap, const char *videoId)
{
   snprintf(path, cap, "%s/%s.mp4", getVideoStagingDir(), videoId);
}

// ---- the remux itself ----

// remux the video (and optional audio) url into one .mp4 at outPath. reads elementary access units from
// the demuxers and writes MP4 samples, checking cancelAll each unit. 0 on a complete file, -1 otherwise
// (the partial file is removed by the caller).
static int remux(const char *videoUrl, const char *audioUrl, const char *outPath)
{
   VideoDemuxer *video = (VideoDemuxer *)malloc(sizeof *video);
   VideoDemuxer *audio = audioUrl ? (VideoDemuxer *)malloc(sizeof *audio) : NULL;
   if (!video || (audioUrl && !audio)) { free(video); free(audio); return -1; }

   int result = -1;
   int videoOpen = 0, hasAudio = 0;
   Mp4Muxer mux;

   if (openVideoDemuxer(video, videoUrl) != 0) { logError("[dl] video demux open failed\n"); goto cleanup; }
   videoOpen = 1;
   if (audio && openAudioDemuxer(audio, audioUrl) == 0) hasAudio = 1;

   if (openMp4Muxer(&mux, outPath, &video->h264, video->width, video->height, video->frameDurationNs,
                    hasAudio ? audio->audioRate : 0, hasAudio ? audio->audioChannels : 0) != 0)
      goto cleanup;

   // interleave by presentation time: hold one pending AU from each stream and write whichever comes first.
   VideoAu videoAu;
   AudioAu audioAu;
   int videoRc = readVideoAu(video, &videoAu);
   int audioRc = hasAudio ? readAudioAu(audio, &audioAu) : 0;
   uint64_t total = video->durationNs;
   int failed = 0;

   while (!dl.cancelAll && (videoRc == 1 || audioRc == 1)) {
      if (videoRc == 1 && (audioRc != 1 || videoAu.pts <= audioAu.pts)) {
         if (writeMp4Video(&mux, videoAu.data, videoAu.size, videoAu.pts, videoAu.keyframe)) { failed = 1; break; }
         if (total) { uint64_t done = videoAu.pts * 100 / total; dl.percent = done > 100 ? 100 : (int)done; }
         videoRc = readVideoAu(video, &videoAu);
         if (videoRc < 0) { failed = 1; break; }
      } else {
         if (writeMp4Audio(&mux, audioAu.data, audioAu.size, audioAu.pts)) { failed = 1; break; }
         audioRc = readAudioAu(audio, &audioAu);
         if (audioRc < 0) { failed = 1; break; }
      }
   }

   if (closeMp4Muxer(&mux)) failed = 1;
   if (!failed && !dl.cancelAll) { dl.percent = 100; result = 0; }

cleanup:
   if (videoOpen) closeVideoDemuxer(video);
   if (hasAudio) closeVideoDemuxer(audio);
   free(video);
   free(audio);
   return result;
}

static void runAppDownload(const QueueItem *item, const StreamInfo *info);   // below
static void runXmbDownload(const QueueItem *item, StreamInfo *info);         // below
static void finishJob(const char *videoId);                                  // below

static void runOneDownload(const QueueItem *item)
{
   if (item->remuxLocal) { runXmbDownload(item, NULL); return; }   // the app's own copy, no YouTube needed
   StreamInfo *info = (StreamInfo *)malloc(sizeof *info);
   const Extractor *extractor = findExtractor(item->videoId);
   if (!info || !extractor || extractor->extract(item->videoId, info) != 0 || info->formatCount == 0) {
      logError("[dl] resolve failed for %s\n", item->videoId);
      free(info);
      return;
   }
   if (item->quality != DOWNLOAD_XMB) { runAppDownload(item, info); free(info); return; }
   runXmbDownload(item, info);
   free(info);
}

// the XMB's MP4: remuxed from YouTube's streams (info) or - moving a gallery copy - from that copy's files
static void runXmbDownload(const QueueItem *item, StreamInfo *info)
{
   char localVideo[160], localAudio[160];
   const char *videoUrl, *audioUrl;
   if (item->remuxLocal) {
      DownloadInfo copy;
      if (readDownloadInfo(item->videoId, &copy) != 0) { logError("[dl] %s: the app's copy is gone\n", item->videoId); return; }
      strCopy(localVideo, sizeof localVideo, copy.videoPath);
      strCopy(localAudio, sizeof localAudio, copy.audioPath);
      videoUrl = localVideo;
      audioUrl = localAudio[0] ? localAudio : NULL;
   } else {
      const StreamFormat *video = pickBestVideo(info);
      const StreamFormat *audio = (video && !video->hasAudio) ? pickBestAudio(info) : NULL;
      if (!video) { logError("[dl] no downloadable video for %s\n", item->videoId); return; }
      if (video->isLiveSegmented) { logInfo("[dl] %s is a live stream, not downloadable\n", item->videoId); return; }
      logInfo("[dl] %s for the XMB: video itag %d, audio itag %d\n", item->videoId, video->itag,
              audio ? audio->itag : (video->hasAudio ? video->itag : 0));
      strCopy(localVideo, sizeof localVideo, "");
      videoUrl = video->url;
      audioUrl = audio ? audio->url : NULL;
   }

   char finalPath[MAX_PATH_LEN], partPath[MAX_PATH_LEN];
   buildDownloadPath(finalPath, sizeof finalPath, item->videoId);
   snprintf(partPath, sizeof partPath, "%s.part", finalPath);
   logInfo("[dl] %s -> %s%s\n", item->videoId, finalPath, item->remuxLocal ? " (from the app's copy)" : "");

   // download to a .part file and only publish it on success, so an interrupted or duplicate download can
   // never truncate or delete an already-finished file.
   int rc = remux(videoUrl, audioUrl, partPath);

   if (rc != 0) { deleteFile(partPath); logInfo("[dl] %s %s\n", item->videoId, dl.cancelAll ? "cancelled" : "failed"); }
   else if (renamePath(partPath, finalPath) != 0) { deleteFile(partPath); logError("[dl] %s rename failed\n", item->videoId); }
   else {
      logInfo("[dl] %s done\n", item->videoId);
      queueVideoExport(finalPath, item->title);
      if (item->fromGallery) deleteDownload(item->videoId);   // the XMB has it now (its job goes with it)
      else finishJob(item->videoId);
   }
}

// ---- the app's own downloads (V3.0.0) ----

static char downloadsDir[128];

const char *getDownloadsDir(void)
{
   if (!downloadsDir[0]) snprintf(downloadsDir, sizeof downloadsDir, "%s/downloads", getAppDataDir());
   return downloadsDir;
}

unsigned getDownloadsRevision(void) { return dl.revision; }

static void folderOf(char *out, int cap, const char *videoId) { snprintf(out, cap, "%s/%s", getDownloadsDir(), videoId); }

// ---- jobs: what was asked for and has not finished (V3.0.0) ----

static void jobPath(char *out, int cap, const char *videoId) { snprintf(out, cap, "%s/%s/job.txt", getDownloadsDir(), videoId); }

static void writeJob(const QueueItem *item)
{
   char folder[160], path[200], text[512];
   folderOf(folder, sizeof folder, item->videoId);
   makeDirPath(getDownloadsDir());
   makeDirPath(folder);
   jobPath(path, sizeof path, item->videoId);
   int length = snprintf(text, sizeof text, "title=%s\nchannel=%s\nid=%s\nduration=%s\nquality=%d\nmove=%d\n",
                         item->title, item->author, item->videoId, item->duration, item->quality, item->fromGallery);
   if (length > 0 && length < (int)sizeof text) writeFile(path, text, (uint64_t)length);
}

// done with: the job goes, and its folder too when nothing else is in it (an XMB download keeps no files here)
static void finishJob(const char *videoId)
{
   char folder[160], path[200];
   jobPath(path, sizeof path, videoId);
   deleteFile(path);
   folderOf(folder, sizeof folder, videoId);
   removeDirPath(folder);   // fails, harmlessly, while a finished download's files are in it
}

// the best stream of the kind asked for: VP9 above 1080p (up to the 1440p TEE VP9 takes), or H.264 at 1080p and
// below - the most lines first, then the most frames. NULL when the video has none.
static const StreamFormat *pickAppVideo(const StreamInfo *info, int wantVp9)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *f = &info->formats[i];
      if (!f->hasVideo || f->hasAudio || f->needsCipher || !f->url[0] || f->isLiveSegmented) continue;
      if (wantVp9 ? !(f->isVp9 && f->height > 1080 && f->height <= 1440)
                  : (f->isVp9 || strcmp(f->container, "mp4") != 0 || f->height > 1080 || f->width > 1920)) continue;
      if (!best || f->height > best->height || (f->height == best->height && f->fps > best->fps)) best = f;
   }
   return best;
}

// one stream, byte for byte, into path (a .part the caller renames). The percent runs from `from` to `to` over it.
// 0 when every byte arrived; *bytes is how many did.
static int copyStream(const char *url, const char *path, int from, int to, long long *bytes)
{
   VideoSource source;
   *bytes = 0;
   if (openVideoSource(&source, url) != 0) { logError("[dl] could not open a stream\n"); return -1; }
   uint64_t total = getVideoSourceSize(&source);
   VfsFile file;
   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file) != 0) {
      logError("[dl] could not write %s\n", path);
      closeVideoSource(&source);
      return -1;
   }
   const int cap = 256 * 1024;
   uint8_t *buffer = (uint8_t *)malloc(cap);
   uint64_t done = 0;
   int failed = !buffer;
   while (!failed && !dl.cancelAll && (!total || done < total)) {
      uint64_t want = total && total - done < (uint64_t)cap ? total - done : (uint64_t)cap;
      int64_t got = readVideoSource(&source, buffer, want);
      if (got <= 0) { if (total) failed = 1; break; }
      if (writeFs(&file, buffer, (uint64_t)got) != got) { failed = 1; break; }
      done += (uint64_t)got;
      if (total) dl.percent = from + (int)((uint64_t)(to - from) * done / total);
   }
   free(buffer);
   if (closeFs(&file) != 0) failed = 1;
   closeVideoSource(&source);
   *bytes = (long long)done;
   return failed || dl.cancelAll || !done ? -1 : 0;
}

// the date as the console clock has it, YYYY-MM-DD (days -> civil date, Howard Hinnant's algorithm)
static void todayText(char *out, int cap, long long *epoch)
{
   sys_time_sec_t sec = 0;
   sys_time_nsec_t nsec = 0;
   sys_time_get_current_time(&sec, &nsec);
   *epoch = (long long)sec;
   long long z = (long long)sec / 86400 + 719468;
   long long era = (z >= 0 ? z : z - 146096) / 146097;
   long long doe = z - era * 146097;
   long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
   long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
   snprintf(out, cap, "%04d-%02d-%02d", (int)(y + (m <= 2)), (int)m, (int)d);
}

// 1440p60 / 1080p60 for the Downloads page: the video stream (95 % of the bar) and the AAC (the rest) as YouTube
// serves them, into the video's folder; info.txt last, so a half download never shows
static void runAppDownload(const QueueItem *item, const StreamInfo *info)
{
   const StreamFormat *video = item->quality == DOWNLOAD_1440P ? pickAppVideo(info, 1) : NULL;
   if (!video) {
      if (item->quality == DOWNLOAD_1440P) logInfo("[dl] %s has no 1440p VP9: 1080p60 H.264 instead\n", item->videoId);
      video = pickAppVideo(info, 0);
   }
   const StreamFormat *audio = pickBestAudio(info);
   if (!video) { logError("[dl] no stream to download for %s\n", item->videoId); return; }

   char folder[160], videoPath[200], audioPath[200], part[208], infoPath[200];
   folderOf(folder, sizeof folder, item->videoId);
   makeDirPath(getDownloadsDir());
   makeDirPath(folder);
   snprintf(videoPath, sizeof videoPath, "%s/video.%s", folder, video->isVp9 ? "webm" : "mp4");
   snprintf(audioPath, sizeof audioPath, "%s/audio.m4a", folder);
   snprintf(infoPath, sizeof infoPath, "%s/info.txt", folder);
   deleteFile(infoPath);   // a download made again counts only once it is complete again
   logInfo("[dl] %s -> %s (itag %d %dx%d %d fps %s, audio itag %d)\n", item->videoId, folder, video->itag, video->width,
           video->height, video->fps, video->isVp9 ? "VP9" : "H.264", audio ? audio->itag : 0);

   long long videoBytes = 0, audioBytes = 0;
   snprintf(part, sizeof part, "%s.part", videoPath);
   int failed = copyStream(video->url, part, 0, audio ? 95 : 100, &videoBytes) != 0 || renamePath(part, videoPath) != 0;
   if (failed) deleteFile(part);
   if (!failed && audio) {
      snprintf(part, sizeof part, "%s.part", audioPath);
      failed = copyStream(audio->url, part, 95, 100, &audioBytes) != 0 || renamePath(part, audioPath) != 0;
      if (failed) deleteFile(part);
   }
   if (failed) {
      deleteFile(videoPath);
      deleteFile(audioPath);
      removeDirPath(folder);
      logInfo("[dl] %s %s\n", item->videoId, dl.cancelAll ? "cancelled" : "failed");
      return;
   }

   char date[16], text[1024];
   long long epoch = 0;
   todayText(date, sizeof date, &epoch);
   int length = snprintf(text, sizeof text,
      "title=%s\nchannel=%s\nid=%s\nduration=%s\nwidth=%d\nheight=%d\nfps=%d\nitag=%d\nvideo=%s\naudio=%s\n"
      "videoFile=%s\naudioFile=%s\nbytes=%lld\ndate=%s\ntime=%lld\n",
      item->title, item->author, item->videoId, item->duration, video->width, video->height, video->fps, video->itag,
      video->isVp9 ? "VP9" : "H.264", audio ? "AAC" : "", video->isVp9 ? "video.webm" : "video.mp4",
      audio ? "audio.m4a" : "", videoBytes + audioBytes, date, epoch);
   if (length < 0 || length >= (int)sizeof text || writeFile(infoPath, text, (uint64_t)length) != 0) {
      logError("[dl] %s: could not write info.txt\n", item->videoId);
      return;
   }
   dl.percent = 100;
   dl.revision++;
   finishJob(item->videoId);
   logInfo("[dl] %s done: %lld bytes\n", item->videoId, videoBytes + audioBytes);
}

// a "key=value" line of info.txt into out ("" when absent)
static void infoValue(const char *text, const char *key, char *out, int cap)
{
   out[0] = 0;
   size_t keyLength = strlen(key);
   for (const char *line = text; line && *line; ) {
      if (strncmp(line, key, keyLength) == 0 && line[keyLength] == '=') {
         const char *value = line + keyLength + 1;
         int n = 0;
         while (value[n] && value[n] != '\n' && value[n] != '\r' && n < cap - 1) { out[n] = value[n]; n++; }
         out[n] = 0;
         return;
      }
      line = strchr(line, '\n');
      if (line) line++;
   }
}

int readDownloadInfo(const char *videoId, DownloadInfo *out)
{
   memset(out, 0, sizeof *out);
   if (!videoId || !videoId[0] || strchr(videoId, '/') || strstr(videoId, "..")) return -1;
   char folder[160], path[200], text[1536], value[64];
   folderOf(folder, sizeof folder, videoId);
   snprintf(path, sizeof path, "%s/info.txt", folder);
   int length = readFile(path, text, sizeof text - 1);
   if (length <= 0) return -1;
   text[length] = 0;
   infoValue(text, "title", out->title, sizeof out->title);
   infoValue(text, "channel", out->channel, sizeof out->channel);
   infoValue(text, "duration", out->duration, sizeof out->duration);
   infoValue(text, "date", out->date, sizeof out->date);
   strCopy(out->videoId, sizeof out->videoId, videoId);
   infoValue(text, "width", value, sizeof value);  out->width = atoi(value);
   infoValue(text, "height", value, sizeof value); out->height = atoi(value);
   infoValue(text, "fps", value, sizeof value);    out->fps = atoi(value);
   infoValue(text, "itag", value, sizeof value);   out->itag = atoi(value);
   infoValue(text, "bytes", value, sizeof value);  out->bytes = atoll(value);
   infoValue(text, "time", value, sizeof value);   out->time = atoll(value);
   infoValue(text, "video", value, sizeof value);  out->isVp9 = strcmp(value, "VP9") == 0;
   infoValue(text, "videoFile", value, sizeof value);
   if (!value[0] || strchr(value, '/')) return -1;
   snprintf(out->videoPath, sizeof out->videoPath, "%s/%s", folder, value);
   infoValue(text, "audioFile", value, sizeof value);
   if (value[0] && !strchr(value, '/')) snprintf(out->audioPath, sizeof out->audioPath, "%s/%s", folder, value);
   return fileExists(out->videoPath) ? 0 : -1;
}

int listDownloads(SearchResults *out)
{
   memset(out, 0, sizeof *out);
   long long times[MAX_SEARCH_RESULTS];
   VfsDir dir;
   if (openDir(getDownloadsDir(), &dir) != 0) return 0;
   char name[64];
   VfsEntryType type;
   while (out->count < MAX_SEARCH_RESULTS && readDir(&dir, name, sizeof name, &type) == 1) {
      if (type != VFS_ENTRY_DIR || name[0] == '.') continue;
      DownloadInfo info;
      if (readDownloadInfo(name, &info) != 0) continue;
      SearchResult item;
      memset(&item, 0, sizeof item);
      strCopy(item.videoId, sizeof item.videoId, info.videoId);
      strCopy(item.title, sizeof item.title, info.title[0] ? info.title : info.videoId);
      strCopy(item.author, sizeof item.author, info.channel);
      strCopy(item.duration, sizeof item.duration, info.duration);
      // what the page is for: in which form it was downloaded - "1440p60 · VP9", "812 MB · 2026-11-11"
      snprintf(item.views, sizeof item.views, "%dp%d \xC2\xB7 %s", info.height, info.fps, info.isVp9 ? "VP9" : "H.264");
      long long mb = (info.bytes + 512 * 1024) / (1024 * 1024);
      if (mb >= 1024) snprintf(item.published, sizeof item.published, "%d.%d GB \xC2\xB7 %s", (int)(mb / 1024),
                               (int)(mb % 1024 * 10 / 1024), info.date);
      else            snprintf(item.published, sizeof item.published, "%d MB \xC2\xB7 %s", (int)mb, info.date);
      // newest first
      int at = out->count;
      while (at > 0 && times[at - 1] < info.time) { out->items[at] = out->items[at - 1]; times[at] = times[at - 1]; at--; }
      out->items[at] = item;
      times[at] = info.time;
      out->count++;
   }
   closeDir(&dir);
   return out->count;
}

int deleteDownload(const char *videoId)
{
   if (!videoId || !videoId[0] || strchr(videoId, '/') || strstr(videoId, "..")) return -1;
   char folder[160], path[200];
   folderOf(folder, sizeof folder, videoId);
   static const char *files[] = { "info.txt", "job.txt", "video.webm", "video.mp4", "audio.m4a", "video.webm.part",
                                  "video.mp4.part", "audio.m4a.part" };
   for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
      snprintf(path, sizeof path, "%s/%s", folder, files[i]);
      deleteFile(path);
   }
   int rc = removeDirPath(folder);
   dl.revision++;
   logInfo("[dl] %s deleted%s\n", videoId, rc == 0 ? "" : " (its folder stayed)");
   return rc;
}

// a folder's job, read back into a queue item: 0 when there is one
static int readJob(const char *videoId, QueueItem *item)
{
   char path[200], text[768], value[16];
   jobPath(path, sizeof path, videoId);
   int length = readFile(path, text, sizeof text - 1);
   if (length <= 0) return -1;
   text[length] = 0;
   memset(item, 0, sizeof *item);
   strCopy(item->videoId, sizeof item->videoId, videoId);
   infoValue(text, "title", item->title, sizeof item->title);
   infoValue(text, "channel", item->author, sizeof item->author);
   infoValue(text, "duration", item->duration, sizeof item->duration);
   infoValue(text, "quality", value, sizeof value);
   item->quality = atoi(value);
   if (item->quality < 0 || item->quality >= DOWNLOAD_QUALITY_COUNT) item->quality = DOWNLOAD_XMB;
   if (!item->title[0]) strCopy(item->title, sizeof item->title, videoId);
   return 0;
}

// the folders that hold a job, as their video ids (at most `cap`); a job beside a complete info.txt only outlived
// its download by a moment and is dropped on the spot
static int collectJobs(char ids[][16], int cap)
{
   VfsDir dir;
   if (openDir(getDownloadsDir(), &dir) != 0) return 0;
   char name[64];
   VfsEntryType type;
   int count = 0;
   while (count < cap && readDir(&dir, name, sizeof name, &type) == 1) {
      if (type != VFS_ENTRY_DIR || name[0] == '.' || strlen(name) >= 16) continue;
      char path[200];
      jobPath(path, sizeof path, name);
      if (!fileExists(path)) continue;
      DownloadInfo info;
      if (readDownloadInfo(name, &info) == 0) { deleteFile(path); continue; }
      strCopy(ids[count], 16, name);
      count++;
   }
   closeDir(&dir);
   return count;
}

int findInterruptedDownloads(char *firstTitle, int titleCap)
{
   char ids[MAX_QUEUE][16];
   int count = collectJobs(ids, MAX_QUEUE);
   if (firstTitle && titleCap > 0) {
      firstTitle[0] = 0;
      QueueItem item;
      if (count > 0 && readJob(ids[0], &item) == 0) strCopy(firstTitle, titleCap, item.title);
   }
   if (count) logInfo("[dl] %d download(s) did not finish\n", count);
   return count;
}

// what a cut-short download left: its half-written files here, and an XMB one's .part in the export's staging folder
static void dropPartials(const char *videoId)
{
   char folder[160], path[220];
   folderOf(folder, sizeof folder, videoId);
   static const char *files[] = { "video.webm.part", "video.mp4.part", "audio.m4a.part", "video.webm", "video.mp4",
                                  "audio.m4a" };
   for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
      snprintf(path, sizeof path, "%s/%s", folder, files[i]);
      deleteFile(path);
   }
   snprintf(path, sizeof path, "%s/%s.mp4.part", getVideoStagingDir(), videoId);
   deleteFile(path);
}

void restartInterruptedDownloads(void)
{
   char ids[MAX_QUEUE][16];
   int count = collectJobs(ids, MAX_QUEUE);
   for (int i = 0; i < count; i++) {
      QueueItem job;
      if (readJob(ids[i], &job) != 0) continue;
      dropPartials(ids[i]);
      SearchResult item;
      memset(&item, 0, sizeof item);
      strCopy(item.videoId, sizeof item.videoId, job.videoId);
      strCopy(item.title, sizeof item.title, job.title);
      strCopy(item.author, sizeof item.author, job.author);
      strCopy(item.duration, sizeof item.duration, job.duration);
      enqueueDownloadAs(&item, (DownloadQuality)job.quality);   // writes the job again
      logInfo("[dl] %s queued again\n", ids[i]);
   }
}

void deleteInterruptedDownloads(void)
{
   char ids[MAX_QUEUE][16];
   int count = collectJobs(ids, MAX_QUEUE);
   for (int i = 0; i < count; i++) {
      dropPartials(ids[i]);
      finishJob(ids[i]);
      logInfo("[dl] %s: the unfinished download was deleted\n", ids[i]);
   }
}

// ---- worker + queue ----

// one persistent low-priority worker: drains the queue one item at a time, then parks until more arrive or
// the app exits. spawned lazily on the first enqueue and joined once in shutdownDownloads.
static void downloadWorker(uint64_t arg)
{
   (void)arg;
   for (;;) {
      QueueItem item;
      lock(&dl.mutex);
      if (dl.cancelAll) { dl.active = 0; unlock(&dl.mutex); break; }
      if (dl.count == 0) { dl.active = 0; unlock(&dl.mutex); sleepMs(100); continue; }
      item = dl.queue[0];
      for (int i = 1; i < dl.count; i++) dl.queue[i - 1] = dl.queue[i];
      dl.count--;
      dl.active = 1;
      dl.percent = 0;
      strCopy(dl.activeTitle, sizeof dl.activeTitle, item.title);
      strCopy(dl.activeId, sizeof dl.activeId, item.videoId);
      unlock(&dl.mutex);

      runOneDownload(&item);
      lock(&dl.mutex);
      dl.batchDone++;
      unlock(&dl.mutex);
   }
   exitThread();
}

void enqueueDownload(const SearchResult *item) { enqueueDownloadAs(item, DOWNLOAD_XMB); }

static int enqueueItem(const SearchResult *item, DownloadQuality quality, int fromGallery, int remuxLocal);

void enqueueDownloadAs(const SearchResult *item, DownloadQuality quality) { enqueueItem(item, quality, 0, 0); }

int moveDownloadToXmb(const char *videoId)
{
   DownloadInfo info;
   if (readDownloadInfo(videoId, &info) != 0) return -1;
   SearchResult item;
   memset(&item, 0, sizeof item);
   strCopy(item.videoId, sizeof item.videoId, info.videoId);
   strCopy(item.title, sizeof item.title, info.title);
   strCopy(item.author, sizeof item.author, info.channel);
   strCopy(item.duration, sizeof item.duration, info.duration);
   // the XMB's player keeps up with H.264 at 30 fps, or at 720p and below: such a copy is remuxed as it is
   int remuxLocal = !info.isVp9 && (info.fps <= 30 || info.height <= 720);
   logInfo("[dl] %s to the XMB: %s\n", videoId, remuxLocal ? "remuxing the app's copy" : "downloading the XMB's MP4");
   return enqueueItem(&item, DOWNLOAD_XMB, 1, remuxLocal);
}

static int enqueueItem(const SearchResult *item, DownloadQuality quality, int fromGallery, int remuxLocal)
{
   if (!dl.initialized || !item->videoId[0]) return -1;

   int spawn = 0, duplicate = 0, full = 0;
   lock(&dl.mutex);
   if (dl.active && strcmp(dl.activeId, item->videoId) == 0) duplicate = 1;
   for (int i = 0; i < dl.count && !duplicate; i++)
      if (strcmp(dl.queue[i].videoId, item->videoId) == 0) duplicate = 1;

   if (!dl.cancelAll && !duplicate) {
      if (dl.count >= MAX_QUEUE) full = 1;
      else {
         if (!dl.active && dl.count == 0) dl.batchTotal = dl.batchDone = 0;   // a new batch
         dl.batchTotal++;
         strCopy(dl.queue[dl.count].videoId, sizeof dl.queue[dl.count].videoId, item->videoId);
         strCopy(dl.queue[dl.count].title,   sizeof dl.queue[dl.count].title,   item->title);
         strCopy(dl.queue[dl.count].author,  sizeof dl.queue[dl.count].author,  item->author);
         strCopy(dl.queue[dl.count].duration, sizeof dl.queue[dl.count].duration, item->duration);
         dl.queue[dl.count].quality = (int)quality;
         dl.queue[dl.count].fromGallery = fromGallery;
         dl.queue[dl.count].remuxLocal = remuxLocal;
         writeJob(&dl.queue[dl.count]);   // from now until it is done (findInterruptedDownloads)
         dl.count++;
         if (!dl.workerSpawned) { dl.workerSpawned = 1; spawn = 1; }
      }
   }
   unlock(&dl.mutex);

   if (duplicate) logInfo("[dl] %s already queued or downloading, skipped\n", item->videoId);
   else if (full) logWarn("[dl] queue full, %s skipped\n", item->videoId);

   if (spawn && spawnJoinableThread(&dl.worker, downloadWorker, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-dl") != 0) {
      lock(&dl.mutex); dl.workerSpawned = 0; unlock(&dl.mutex);
      logError("[dl] worker spawn failed\n");
      return -1;
   }
   return duplicate || full ? -1 : 0;
}

// ---- overlay ----

// "Downloading "<title>" NN%" with the title truncated so it can't fill the screen; "(+N)" when more are
// queued. The title part is one label; the percent and the "(+N)" are drawn after it (drawDownloadOverlayAt),
// so neither a count nor a percent step has to re-render the title.
static void formatOverlay(char *out, int cap)
{
   char clipped[sizeof dl.activeTitle];
   strCopy(clipped, sizeof clipped, dl.activeTitle);
   truncateUtf8(clipped, DISPLAY_TITLE_MAX);
   int truncated = strcmp(clipped, dl.activeTitle) != 0;

   snprintf(out, cap, "Downloading \"%s%s\"", clipped, truncated ? "..." : "");
}

void updateDownloadOverlay(void)
{
   if (!dl.initialized || !dl.active) return;
   char text[128];
   formatOverlay(text, sizeof text);
   // a video title: drawn raw, and all of it in FONT_POP when Manrope lacks one of its characters. It only
   // rasterises when the title changes, the "(+N)" when the count does, and the percent never (its digits
   // were rendered once, in initDownloads).
   uiSetTextLabel(&overlayLabel, OVERLAY_WEIGHT, text);

   char queued[16] = "";
   int count = dl.count;
   if (count > 0) snprintf(queued, sizeof queued, "(+%d)", count);
   setLabelText(&queueLabel, queued);
   snprintf(percentText, sizeof percentText, " %d%%", dl.percent);
}

int getDownloadStatus(int *index, int *total, int *percent)
{
   if (!dl.initialized) return 0;
   lock(&dl.mutex);
   int busy = dl.active || dl.count > 0;
   int done = dl.batchDone, all = dl.batchTotal, now = dl.percent;
   unlock(&dl.mutex);
   if (!busy || all <= 0) return 0;
   if (index)   *index   = done + 1 > all ? all : done + 1;
   if (total)   *total   = all;
   if (percent) *percent = now < 0 ? 0 : now > 100 ? 100 : now;
   return 1;
}

static int overlayShown(void)
{
   return dl.initialized && dl.active && overlayLabel.tt.tex.w > 0;
}

// the chip's parts, left to right: the arrow, the title, the percent and - while more wait - the "(+N)"
static int iconSpan(void)    { return overlayIcon.tex ? OVERLAY_ICON + OVERLAY_ICON_GAP : 0; }
static int queueSpan(void)
{
   return queueLabel.tt.tex.w > 0 ? uiGetDigitsWidth(&percentDigits, " ") + queueLabel.tt.tex.w : 0;
}

int getDownloadOverlayWidth(void)
{
   if (!overlayShown()) return 0;
   return OVERLAY_PAD_L + iconSpan() + overlayLabel.tt.tex.w + uiGetDigitsWidth(&percentDigits, percentText)
          + queueSpan() + OVERLAY_PAD_R;
}

void drawDownloadOverlayAt(int x, int y, UiAlign align)
{
   int chipW = getDownloadOverlayWidth();
   if (chipW == 0) return;
   if (align == UI_ALIGN_RIGHT)       x -= chipW;
   else if (align == UI_ALIGN_CENTER) x -= chipW / 2;

   // the pill and, 2 px inside its edge, the pill again in surface-2: the edge is left as a hairline ring
   uiFillPill(x, y, chipW, OVERLAY_H, overlayEdge);
   uiFillPill(x + OVERLAY_EDGE, y + OVERLAY_EDGE, chipW - 2 * OVERLAY_EDGE, OVERLAY_H - 2 * OVERLAY_EDGE,
              activeTheme->surface);

   int textX = x + OVERLAY_PAD_L;
   if (overlayIcon.tex) {
      // the arrow's ink centred in its 22-px box and on the chip: drawIcon places the glyph's em box
      drawIcon(&overlayIcon, textX + (OVERLAY_ICON - overlayIcon.tex->w) / 2 - overlayIcon.offsetX,
               y + (OVERLAY_H - overlayIcon.tex->h) / 2 - overlayIcon.offsetY, activeTheme->accent);
      textX += iconSpan();
   }

   // the three texts share one baseline: the title may have fallen back to FONT_POP, which puts its capitals
   // at another height in its texture than Manrope does
   int digitsY  = uiTextYForCenter(percentDigits.font, percentDigits.size, y, OVERLAY_H);
   int baseline = digitsY + uiGetTextMetrics(percentDigits.font, percentDigits.size).baseline;
   uiDrawLabelAt(&overlayLabel, textX, uiLabelYForBaseline(&overlayLabel, baseline));
   textX += overlayLabel.tt.tex.w;
   uiDrawDigits(&percentDigits, textX, digitsY, percentText, activeTheme->accent);
   textX += uiGetDigitsWidth(&percentDigits, percentText);
   if (queueLabel.tt.tex.w > 0)
      uiDrawLabelAt(&queueLabel, textX + uiGetDigitsWidth(&percentDigits, " "), uiLabelYForBaseline(&queueLabel, baseline));
}

// Above the right end of the hint row, inside the safe area. Until v0.13 it sat 6 px from the screen's top
// right corner - outside the safe area, where a TV's overscan can cut it off. On Home that strip is free: the
// list pages' cards end well above it. A screen whose content runs down to its hint row places the chip
// itself (drawDownloadOverlayAt).
void drawDownloadOverlay(int screenWidth)
{
   drawDownloadOverlayAt(screenWidth - UI_SAFE_X, UI_HINTS_BOTTOM_Y - OVERLAY_GAP - OVERLAY_H, UI_ALIGN_RIGHT);
}

// ---- lifecycle ----

void initDownloads(void)
{
   memset(&dl, 0, sizeof dl);
   createLock(&dl.mutex);
   // the shared fonts (ui-fonts.h), open since initUiFonts; the digits need gfx, up since initGfx
   uiInitTextLabel(&overlayLabel, OVERLAY_WEIGHT, 0, 0, OVERLAY_TEXT_MAX_W, OVERLAY_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   // the queue count is secondary to what is downloading now: text-2
   uiInitLabel(&queueLabel, OVERLAY_WEIGHT, 0, 0, AUTO, OVERLAY_TEXT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   uiInitDigits(&percentDigits, PERCENT_WEIGHT, OVERLAY_TEXT_SIZE);
   strCopy(percentText, sizeof percentText, " 0%");
   // the icon font is loaded before this (main.c); a glyph that fails to rasterise leaves the arrow out
   initIcon(&overlayIcon, ICON_DOWNLOAD, OVERLAY_ICON);
   overlayEdge = interpolateColor(activeTheme->surface, uiWithAlpha(activeTheme->hairline, 0xFF),
                                  (float)(activeTheme->hairline >> 24) / 255.0f);
   dl.initialized = 1;
}

void shutdownDownloads(void)
{
   if (!dl.initialized) return;
   dl.cancelAll = 1;
   if (dl.workerSpawned) joinThread(dl.worker);
   dl.count = 0;
   finishGfx();   // the overlay labels may have been sampled last frame
   freeLabel(&overlayLabel);
   freeLabel(&queueLabel);
   uiFreeDigits(&percentDigits);
   freeIcon(&overlayIcon);   // the texture belongs to the icon font's cache, which main.c frees
   // no closeFont: the fonts are shared (main.c closes them in termUiFonts, after this)
   destroyLock(&dl.mutex);
   dl.initialized = 0;
}

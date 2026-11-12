#pragma once

// downloads - background video downloads. R3 on a list page or a search result queues a video; a single
// low-priority worker drains the queue one at a time, remuxing the same H.264 + AAC streams the player would
// show into one .mp4, then handing it to video-export so it turns up in the XMB's Video column. The active
// download's progress shows as an Ember chip on the browse screens, inside the TV's safe area: by default
// bottom-right, just above their hint row, or wherever a screen puts it with drawDownloadOverlayAt.
// Everything runs off the UI thread; the queue is cancelled and cleared on app exit.

#include "extractor.h"   // SearchResult
#include "ui-draw.h"     // UiAlign

#define DOWNLOAD_OVERLAY_H 44   // the chip's height: a pill, as tall as the Ember filter chips (Bild 6)

// set up the worker state + overlay; call after initUiFonts (the overlay's labels use the shared fonts),
// after initIconFont (its download icon) and after initGfx (its percent digits are textures)
void initDownloads(void);
void shutdownDownloads(void);   // cancel + clear the queue, join the worker, free the overlay; call at exit

void enqueueDownload(const SearchResult *item);   // add a video to the download queue (spawns the worker if idle)

// (V3.0.0) in which form a video is downloaded (the R3 page, download-choice.h). Keep the order: it is the page's
// rows and settings.txt's download-quality= values (0, 1, 2).
typedef enum {
   DOWNLOAD_1440P,      // 1440p60 VP9 (+ AAC) for the app's Downloads page; a video without it gets DOWNLOAD_1080P60
   DOWNLOAD_1080P60,    // 1080p60 H.264 (+ AAC) for the Downloads page (the TEE Decoder plays it)
   DOWNLOAD_XMB,        // the MP4 the XMB's own player keeps up with (1080p30 / 720p60), into its Video column
   DOWNLOAD_QUALITY_COUNT
} DownloadQuality;
void enqueueDownloadAs(const SearchResult *item, DownloadQuality quality);

// The app's downloads: one folder each under getDownloadsDir(), the streams as YouTube served them (no remux: the
// player reads WebM and MP4 alike) and an info.txt, written last, which makes the folder count.
#define LOCAL_INPUT_PREFIX "local:"   // playVideo("local:<videoId>") plays a download (extractors/local.c)
typedef struct {
   char title[160], channel[48], videoId[16], duration[12], date[16];
   int  width, height, fps, itag, isVp9;
   long long bytes;
   long long time;                   // when it finished (the console clock), for the newest-first order
   char videoPath[160], audioPath[160];
} DownloadInfo;
const char *getDownloadsDir(void);
int  readDownloadInfo(const char *videoId, DownloadInfo *out);   // 0 when the download is complete on disk
int  listDownloads(SearchResults *out);                          // newest first; the count (0 = none)
int  deleteDownload(const char *videoId);                        // 0 when its folder is gone
unsigned getDownloadsRevision(void);                             // moves when a download lands or is deleted

// (V3.0.0) downloads that never finished. Every download asked for leaves a job.txt in its folder from the moment
// it is queued until it is done, so one that a power cut, a crash or leaving the app cut short - or that was still
// waiting in the queue - is still there at the next start. Call these at start-up, before anything is queued.
// The count of such jobs (0 = none), and the first one's title for the question.
int  findInterruptedDownloads(char *firstTitle, int titleCap);
void restartInterruptedDownloads(void);   // queue each one again (from the start, in the form it was asked for)
void deleteInterruptedDownloads(void);    // throw them away, half-written files and all

// (V3.0.0) a download of the app's own moved into the XMB's Video column: its H.264 streams remuxed into one MP4
// where the XMB's player keeps up with them (30 fps, or 720p and below), else the XMB's MP4 downloaded afresh
// (VP9 and 1080p60 do not play there). Once the XMB has it, the app's copy is deleted - the app does not see what the
// system took. 0 when it was queued.
int  moveDownloadToXmb(const char *videoId);

void updateDownloadOverlay(void);         // refresh the "Downloading ..." label; call each frame in a screen update
// draws it bottom-right, above the hint row (UI_HINTS_BOTTOM_Y), if a download is active; call in a screen draw
void drawDownloadOverlay(int screenWidth);
// The same chip with its top at y and its left edge, middle or right edge at x (align), for a screen whose
// content reaches down to its hint row - the search results run to just above it. Draws nothing while no
// download is running.
void drawDownloadOverlayAt(int x, int y, UiAlign align);
// the chip's width as it would be drawn now, 0 while no download is running
int  getDownloadOverlayWidth(void);

// (V3.0.0) for the rail's download indicator: 1 while downloads run or wait, with the running one's place in
// this batch (*index of *total - "1/10"; the batch starts over once the queue has run empty) and its percent
int  getDownloadStatus(int *index, int *total, int *percent);
// (V3.0.0) the Downloads page lists the app's own downloads still on their way too (listDownloads, at the top,
// SearchResult.downloadState): 1 while this one downloads (*percent), 2 while it waits in the queue, 0 otherwise
int  getDownloadProgress(const char *videoId, int *percent);
// (V3.0.0) stops a download of the app's own that runs or waits, and throws its half-written files away (an earlier
// complete copy of the same video stays). 0 when there was one.
int  cancelDownload(const char *videoId);

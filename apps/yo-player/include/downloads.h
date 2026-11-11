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

#pragma once

// downloads - background video downloads. Square/R3 on a grid queues a video; a single low-priority worker
// drains the queue one at a time, remuxing the same H.264 + AAC streams the player would show into one
// .mp4, then handing it to video-export so it turns up in the XMB's Video column. The active download's
// progress shows bottom-right on the browse screens, just above their hint row and inside the TV's safe
// area. Everything runs off the UI thread; the queue is cancelled and cleared on app exit.

#include "extractor.h"   // SearchResult

// set up the worker state + overlay; call after initUiFonts (the overlay's labels use the shared fonts) and
// after initGfx (its percent digits are textures)
void initDownloads(void);
void shutdownDownloads(void);   // cancel + clear the queue, join the worker, free the overlay; call at exit

void enqueueDownload(const SearchResult *item);   // add a video to the download queue (spawns the worker if idle)

void updateDownloadOverlay(void);         // refresh the "Downloading ..." label; call each frame in a screen update
// draws it bottom-right, above the hint row (UI_HINTS_BOTTOM_Y), if a download is active; call in a screen draw
void drawDownloadOverlay(int screenWidth);

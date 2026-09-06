#pragma once

// video-grid - a reusable, pad-navigable list of video results. Owns the whole display engine: a background
// worker that fetches result pages + thumbnail jpegs, windowed materialisation (only the on-screen band of
// tiles holds a decoded thumbnail texture + rasterised labels, so VRAM stays flat however many results are
// loaded), D-pad navigation, and paging. Screens supply a GridFetchFn (which encodes the mode: trending /
// search / channel + sort) and read back the selection; the grid knows nothing about modes. See search.c /
// home.c for callers.
//
// Results are shown a PAGE at a time rather than as one long scroll: a page is exactly one screenful, R2/L2
// turn pages, and stepping off the top or bottom edge turns one too. A screen draws the position from
// gridPageIndex / gridPageCount.
//
// Two presentations of the same data (setGridView):
//   GRID_VIEW_GRID - GRID_COLS thumbnails per row, title underneath. Dense; the original look.
//   GRID_VIEW_LIST - one result per row (thumbnail left, title + meta beside it) down the left of the screen
//                    with a large preview of the selection on the right, the way a desktop browser lists them.

#include "extractor.h"          // SearchResults / SearchResult
#include "font.h"               // Font
#include "ui/label.h"           // Label
#include "button-repeat.h"      // ButtonRepeat
#include "thread.h"             // sys_ppu_thread_t
#include <stdint.h>

#define GRID_COLS     4         // columns in GRID_VIEW_GRID (list view is always 1)
#define GRID_THREADS  4         // parallel thumbnail fetch threads

typedef enum { GRID_RUNNING, GRID_READY, GRID_EMPTY } GridStage;
typedef enum { GRID_VIEW_GRID, GRID_VIEW_LIST } GridView;

// what the background worker is doing this spawn. LOAD/CACHE fetch page 1 (CACHE restores kept results first);
// MORE appends the next continuation page; THUMBS extends thumbnails to the current window frontier.
typedef enum { JOB_LOAD, JOB_CACHE, JOB_MORE, JOB_THUMBS } GridJob;

// fetch one page into out: token NULL = first page, else a continuation / sort token. 0 ok, negative error.
typedef int (*GridFetchFn)(const char *token, SearchResults *out, void *user);

// one thumbnail: jpeg fetched by the worker and kept; the texture is decoded only while the tile is in the
// visible window (tex.offset != 0) and freed when it scrolls away, re-decoding from the kept jpeg on return.
typedef enum { THUMB_NONE, THUMB_FETCHED, THUMB_FAILED } ThumbState;
typedef struct {
   volatile ThumbState state;
   uint8_t   *jpeg;
   int        jpegLen;
   GfxTexture tex;
} GridThumb;

typedef struct {
   Font            *font;
   int              x, y, width, height;                   // grid rectangle on screen
   GridView         view;
   int              cols;                                  // 1 in list view, GRID_COLS in grid view
   int              tileW, thumbH, rowStep, visibleRows;   // derived geometry (tileW = thumbnail width)
   int              textX, textW;                          // per-row title/meta column, relative to a row's left
   int              previewX, previewW;                    // list view: the preview pane (0 wide in grid view)

   SearchResults    results;
   GridThumb        thumbs[MAX_SEARCH_RESULTS];
   Label            rows[MAX_SEARCH_RESULTS];              // per-tile title
   Label            metas[MAX_SEARCH_RESULTS];             // per-tile "author - views - age"
   Label            durations[MAX_SEARCH_RESULTS];         // per-tile duration / LIVE badge
   Label            watchLaterBadge;                       // shared "Watch Later" tag drawn on queued tiles
   Label            previewTitle, previewMeta;             // list view: the selection, shown large on the right
   int              previewFor;                            // result index the preview labels hold (-1 = none)

   int              page;                                  // results are shown a page at a time, never scrolled
   int              selected, scrollRow, winStart, winEnd;  // scrollRow is derived: page * visibleRows
   ButtonRepeat     horizontalRepeat, verticalRepeat, pageRepeat;

   // background worker + thumbnail fetch cursor
   volatile GridStage stage;
   volatile int     searchParsed, workerDone, stopWorker;
   GridJob          job;
   int              thumbFetched;                          // thumbnails fetched so far (contiguous from 0)
   int              threadActive, reloadPending;
   sys_ppu_thread_t workerTid;
   volatile int     thumbNext, thumbEnd;
   int              moreEmpty, moreFailed;                 // consecutive fruitless / failed next-page fetches

   GridFetchFn      fetch;
   void            *fetchUser;
   const SearchResults *pendingCached;                     // set = next (re)load restores these instantly
   int              (*isWatched)(const char *videoId);     // fade predicate (NULL = no fade)
   int              (*isWatchLater)(const char *videoId);  // watch-later badge predicate (NULL = no badge)
   int              (*watchedPosition)(const char *videoId);   // seconds watched, for the progress line (NULL = none)
   int              inputSuppressed;                           // pad ignored, but the grid keeps working
} VideoGrid;

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height);
void termVideoGrid(VideoGrid *grid);

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId));
void setGridWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *videoId));

// where a tile's "you got this far" line comes from (seconds; 0 = nothing to draw). The app already stores
// this to resume playback - the line just makes it visible. NULL leaves the tiles as they were.
void setGridProgressSource(VideoGrid *grid, int (*watchedPosition)(const char *videoId));

// Stop the grid reading the pad WITHOUT stopping it working. updateVideoGrid is not only input: it reaps
// the fetch worker, applies a deferred source swap, decodes the thumbnails for the visible window and
// starts the next page. A screen that simply skips the call while some overlay has focus therefore freezes
// loading - so an overlay suppresses input and keeps calling it. Re-enabling also clears the auto-repeat
// timers, or a direction still held from the overlay would fire the moment focus came back.
void setGridInputSuppressed(VideoGrid *grid, int suppressed);

// switch presentation, keeping the results, the thumbnails already fetched and the selection. Re-lays out the
// labels for the new column width, so it is not free - drive it from a button, not per frame.
void setGridView(VideoGrid *grid, GridView view);
GridView gridView(const VideoGrid *grid);

// load a fresh feed from page 1. non-blocking: if a load is already running it's swapped once that finishes,
// so mode/sort switches never freeze the frame.
void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user);
// restore already-known results instantly (grid then only re-fetches thumbnails); fetch/user drive paging.
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user);

void updateVideoGrid(VideoGrid *grid);   // nav + scroll + window + decode + infinite scroll; call each frame
void drawVideoGrid(VideoGrid *grid);
void stopVideoGrid(VideoGrid *grid);     // block until the worker stops (before playback / teardown)

// page position, for the "Page 3 / 12+" readout a screen draws in its header. gridPageCount is how many
// pages are loaded RIGHT NOW - youtube's feeds are continuation-based and never state a total, so the count
// grows as pages arrive. gridHasMorePages says whether more can still be fetched, i.e. whether the count
// shown is a running figure ("12+") or the real end of the feed ("12").
int                  gridPageIndex(const VideoGrid *grid);   // 0-based
int                  gridPageCount(const VideoGrid *grid);   // at least 1
int                  gridHasMorePages(const VideoGrid *grid);
// the ready-made readout: "Page 3 / 12" once the feed has ended, "Page 3 / 12+" while it can still grow.
void                 formatGridPage(const VideoGrid *grid, char *out, int cap);

GridStage            gridStage(const VideoGrid *grid);
int                  gridBusy(const VideoGrid *grid);       // a fetch worker is running (results may still be growing)
int                  gridStopRequested(const VideoGrid *grid); // a source swap asked the worker to stop (bail long fetches)
const SearchResult  *gridSelected(const VideoGrid *grid);   // NULL if empty
int                  gridSelectedIndex(const VideoGrid *grid); // its position, for the autoplay queue
const SearchResults *gridResults(const VideoGrid *grid);    // for caching + channel sort tokens

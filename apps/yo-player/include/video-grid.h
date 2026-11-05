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
// Two presentations of the same data (setGridView), both in the Ember look:
//   GRID_VIEW_GRID - the list pages' cards (Bild 4): GRID_COLS thumbnails per row with rounded corners, a
//                    title of up to two lines and a meta line under each; the focused card grows to 104 %
//                    with a glow and a ring, its title in 800. As many whole rows as fit the rectangle make a
//                    page.
//   GRID_VIEW_LIST - the search results' rows (Bild 5): exactly LIST_ROWS rows a page, each a thumbnail with
//                    the title and "channel . views . age" beside it; the focused row lies on a surface1
//                    plate with a ring and a glow. Row i of the page is the box (x, y + i*step, width, step -
//                    20) with step = height / 3, so the rows fill the rectangle passed to initVideoGrid.
//
// Either view is drawn on activeTheme->appBg (the corner masks are that colour), and the selection's glow
// reaches 81 px past its card or row - a screen draws its header AFTER the grid. Every label takes its own
// font from ui-fonts.h: titles and meta lines are text from YouTube, so each is drawn raw, without what no
// font can draw (uiCleanText: emoji and the like), in Manrope when Manrope has every character left and in
// FONT_POP when not. A title that would still come out mostly blank - one in Thai or Arabic, scripts no font
// of the console has (uiTextIsLegible) - shows its channel name in its place.

#include "extractor.h"          // SearchResults / SearchResult
#include "font.h"               // Font
#include "ui/label.h"           // Label
#include "button-repeat.h"      // ButtonRepeat
#include "thread.h"             // sys_ppu_thread_t
#include <stdint.h>

#define GRID_COLS     4         // columns in GRID_VIEW_GRID (list view is always 1)
#define LIST_ROWS     3         // rows on a page of GRID_VIEW_LIST
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
   Font            *font;                                  // as passed to initVideoGrid; not drawn with any
                                                           // more (see above) - kept so callers still compile
   int              x, y, width, height;                   // grid rectangle on screen
   GridView         view;
   int              cols;                                  // 1 in list view, GRID_COLS in grid view
   int              tileW, thumbH, rowH, rowStep, visibleRows;  // derived geometry: thumbnail size, a card's or a
                                                                //   row's height, the step from one row to the next
   int              textX, textW;                          // title/meta column, relative to a card's or row's left

   SearchResults    results;
   GridThumb        thumbs[MAX_SEARCH_RESULTS];
   Label            titles[MAX_SEARCH_RESULTS];            // per-tile title, its first line (white: tinted when drawn)
   Label            titles2[MAX_SEARCH_RESULTS];           // the second line, when the title needs one (white)
   Label            channels[MAX_SEARCH_RESULTS];          // list view: the channel, ahead of the rest of the meta
   Label            metas[MAX_SEARCH_RESULTS];             // "author . views . age" (list view: ". views . age")
   Label            durations[MAX_SEARCH_RESULTS];         // per-tile duration / LIVE badge
   Label            watchLaterBadge;                       // shared "Watch Later" tag drawn on queued tiles
   Label            watchedBadge;                          // shared "WATCHED" badge on finished videos
   Label            focusTitle, focusTitle2;               // grid view: the focused card's title in 800 (white)
   int              focusTitleIndex;                       // the result they hold (-1 = none yet)
   unsigned char    labelled[MAX_SEARCH_RESULTS];          // the tile's labels are rasterised (it is in the window)

   int              page;                                  // results are shown a page at a time, never scrolled
   int              selected, scrollRow, winStart, winEnd;  // scrollRow is derived: page * visibleRows
   int              focusIndex, focusFrom;                 // the selection the focus look has reached, the one
   uint64_t         focusSinceUs;                          //   it left (-1 = none) and when it changed
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

// font is no longer used for drawing (see the header comment); pass uiGetFont(...) or NULL.
void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height);
void termVideoGrid(VideoGrid *grid);

// A tile counts as watched - dimmed, with the WATCHED badge - when isWatched says so and it shows no progress
// line: a video you stopped half way is "started" (Bild 4), with its progress line and its picture at full
// strength.
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

// Stamps the three auto-repeat timers with "now", so a direction that is already held when this is called
// waits the normal hold delay before it moves anything, while a fresh press still acts at once. For a screen
// coming back to the front (its resume): a button held through the screen that just closed - the player's
// UP/DOWN volume, say - would otherwise read as long overdue and drive the list on the very first frame.
// setGridInputSuppressed does the same when input comes back.
void resetGridRepeat(VideoGrid *grid);

// switch presentation, keeping the results, the thumbnails already fetched and the selection. Re-lays out the
// labels for the new column width, so it is not free - drive it from a button, not per frame.
void setGridView(VideoGrid *grid, GridView view);
GridView gridView(const VideoGrid *grid);

// load a fresh feed from page 1. non-blocking: if a load is already running it's swapped once that finishes,
// so mode/sort switches never freeze the frame.
void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user);
// restore already-known results instantly (grid then only re-fetches thumbnails); fetch/user drive paging.
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user);

// nav + scroll + window + decode + infinite scroll; call each frame. In grid view LEFT and RIGHT keep to the
// selection's row: LEFT stops at the leftmost column - LEFT there is the rail's (home.c) - instead of wrapping
// to the end of the row above, and RIGHT stops at the rightmost one, so a LEFT always takes a RIGHT back.
void updateVideoGrid(VideoGrid *grid);
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

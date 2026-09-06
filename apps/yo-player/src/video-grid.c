// video-grid - the reusable windowed video-result list (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again when the page turns away from it and re-decoded from the kept jpeg on the
// way back, so VRAM stays flat no matter how many results are loaded. The worker is stopped before playback
// so no per-call http fetch overlaps the streaming http client.

#include "video-grid.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "http.h"
#include "dbg.h"                // paging diagnostics
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_GAP    22
#define TILE_TEXT_H 54          // space under each thumbnail for the title + meta line
#define ROW_TITLE   21
#define META_SIZE   15
#define DUR_SIZE    19
#define WATCHLATER_BADGE_SIZE DUR_SIZE
#define MARGIN_ROWS 2           // rows kept materialised above and below the visible band
#define THUMB_LOOKAHEAD_ROWS 3  // rows below the visible band to fetch thumbnails for ahead of scrolling
#define DECODES_PER_FRAME 3
#define THUMB_CAP   (96 * 1024) // one mqdefault jpeg fits easily
#define THUMB_UA    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

// list view: the results column takes this share of the width, the rest is the preview pane. The row
// thumbnail is a share of the column - small enough that five rows fit a 1080p screen, which is about what a
// desktop browser shows, while staying legible from a couch.
#define LIST_COLUMN_PCT 58
#define LIST_THUMB_PCT  27
#define LIST_TEXT_GAP   18
#define LIST_ROW_GAP    14
#define LIST_TITLE      24
#define LIST_META       17
#define PREVIEW_TITLE   28
#define PREVIEW_META    19

#define WATCHED_ALPHA 70        // label alpha for already-watched tiles
#define PROGRESS_BAR_H 5        // "you got this far" line along the bottom edge of a thumbnail

// how many fruitless next-page fetches to accept before declaring the feed finished. YouTube hands back the
// occasional all-duplicate page mid-feed, and a transient fetch failure is not the end of the results either;
// giving up on the first one is what makes a feed stop dead after a page or two.
#define MORE_EMPTY_LIMIT  3
#define MORE_FAIL_LIMIT   4

// ---- thumbnail fetch (worker side) ----

static void thumbFetchThread(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   uint8_t *buffer = (uint8_t *)malloc(THUMB_CAP);
   if (buffer) {
      HttpHeader headers[] = { { "User-Agent", THUMB_UA }, { "Accept-Encoding", "identity" } };
      for (;;) {
         int i = __sync_fetch_and_add(&grid->thumbNext, 1);
         if (i >= grid->thumbEnd || grid->stopWorker) break;

         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", grid->results.items[i].videoId);
         int length = 0, status = 0;
         int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)buffer, THUMB_CAP, &length, &status);
         if (rc == 0 && status == 200 && length > 0) {
            uint8_t *copy = (uint8_t *)malloc(length);
            if (copy) {
               memcpy(copy, buffer, length);
               grid->thumbs[i].jpeg = copy; grid->thumbs[i].jpegLen = length;
               __sync_synchronize();
               grid->thumbs[i].state = THUMB_FETCHED;
            }
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
         }
      }
      free(buffer);
   }
   exitThread();
}

// fetch thumbnails for results [from, to) in parallel; returns once every one has landed or failed.
static void fetchThumbnailsRange(VideoGrid *grid, int from, int to)
{
   grid->thumbNext = from;
   grid->thumbEnd = to;
   sys_ppu_thread_t threads[GRID_THREADS];
   int spawned = 0;
   for (int t = 0; t < GRID_THREADS; t++)
      if (spawnJoinableThread(&threads[spawned], thumbFetchThread, (uint64_t)(uintptr_t)grid,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-thumb") == 0)
         spawned++;
   for (int t = 0; t < spawned; t++) joinThread(threads[t]);
}

// append a fetched page's items, skipping videoIds we already have. sortTokens (channel chips) are kept
// from the first page - continuation pages don't carry them.
static void appendPage(VideoGrid *grid, const SearchResults *page)
{
   for (int i = 0; i < page->count && grid->results.count < MAX_SEARCH_RESULTS; i++) {
      const SearchResult *item = &page->items[i];
      int duplicate = 0;
      for (int j = 0; j < grid->results.count; j++)
         if (strcmp(grid->results.items[j].videoId, item->videoId) == 0) { duplicate = 1; break; }
      if (duplicate) continue;
      grid->results.items[grid->results.count] = *item;
      __sync_synchronize();
      grid->results.count++;
   }
   strCopy(grid->results.continuation, sizeof grid->results.continuation, page->continuation);
}

// how far down to fetch thumbnails: the visible band plus a lookahead, clamped to what's loaded. thumbnails
// are fetched lazily to this frontier instead of all at once, so returning to a category is cheap.
static int thumbFrontier(const VideoGrid *grid)
{
   int frontier = (grid->scrollRow + grid->visibleRows + THUMB_LOOKAHEAD_ROWS) * grid->cols;
   if (frontier > grid->results.count) frontier = grid->results.count;
   return frontier;
}

static void worker(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   switch (grid->job) {
   case JOB_LOAD: {
      int rc = grid->fetch(NULL, &grid->results, grid->fetchUser);
      grid->stage = (rc == 0 && grid->results.count > 0) ? GRID_READY : GRID_EMPTY;
      logInfo("[grid] page 1: rc=%d items=%d nextToken=%d\n", rc, grid->results.count, grid->results.continuation[0] ? 1 : 0);
      __sync_synchronize();
      grid->searchParsed = 1;
      if (grid->stage == GRID_READY) { int target = thumbFrontier(grid); fetchThumbnailsRange(grid, 0, target); grid->thumbFetched = target; }
      break;
   }
   case JOB_CACHE: {   // results already restored; fetch only the first window of thumbnails
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, 0, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_THUMBS: {  // extend thumbnails to the frontier the window has scrolled to
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, grid->thumbFetched, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_MORE: {    // append the next page; its thumbnails are picked up lazily by a later JOB_THUMBS
      SearchResults *page = (SearchResults *)malloc(sizeof *page);
      int base = grid->results.count;
      int rc = page ? grid->fetch(grid->results.continuation, page, grid->fetchUser) : -1;
      if (page && rc == 0) {
         appendPage(grid, page);
         int added = grid->results.count - base;
         logInfo("[grid] more: parsed=%d new=%d total=%d nextToken=%d\n",
                 page->count, added, grid->results.count, grid->results.continuation[0] ? 1 : 0);
         if (added > 0) {
            grid->moreEmpty = grid->moreFailed = 0;
         } else if (++grid->moreEmpty >= MORE_EMPTY_LIMIT) {
            logInfo("[grid] paging ends: %d pages in a row added nothing\n", grid->moreEmpty);
            grid->results.continuation[0] = 0;
         }
         // appendPage adopted the new page's token; without one there is nothing left to ask for
         if (!grid->results.continuation[0] && added > 0) logInfo("[grid] paging ends: feed returned no next token\n");
      } else {
         // the token is still ours to retry with - a failed fetch is not the end of the feed
         if (++grid->moreFailed >= MORE_FAIL_LIMIT) {
            logError("[grid] paging ends: next page failed %d times (rc=%d)\n", grid->moreFailed, rc);
            grid->results.continuation[0] = 0;
         } else {
            logError("[grid] more failed rc=%d (attempt %d), will retry\n", rc, grid->moreFailed);
         }
      }
      free(page);
      break;
   }
   }
   __sync_synchronize();
   grid->workerDone = 1;
   exitThread();
}

static int spawnWorker(VideoGrid *grid)
{
   grid->stopWorker = 0;
   grid->workerDone = 0;
   grid->threadActive = (spawnJoinableThread(&grid->workerTid, worker, (uint64_t)(uintptr_t)grid,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-feed") == 0);
   return grid->threadActive;
}

// ---- geometry ----

// derive the row/column metrics for the current view. Called on init and on every view switch; the labels
// must be re-laid out afterwards (layoutLabels) because their wrap width changes with the text column.
static void computeGeometry(VideoGrid *grid)
{
   if (grid->view == GRID_VIEW_LIST) {
      int column = grid->width * LIST_COLUMN_PCT / 100;
      grid->cols     = 1;
      grid->tileW    = column * LIST_THUMB_PCT / 100;
      grid->thumbH   = grid->tileW * 9 / 16;
      grid->rowStep  = grid->thumbH + LIST_ROW_GAP;
      grid->textX    = grid->tileW + LIST_TEXT_GAP;
      grid->textW    = column - grid->textX;
      grid->previewX = grid->x + column + GRID_GAP;
      grid->previewW = grid->width - column - GRID_GAP;
   } else {
      grid->cols     = GRID_COLS;
      grid->tileW    = (grid->width - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
      grid->thumbH   = grid->tileW * 9 / 16;
      grid->rowStep  = grid->thumbH + TILE_TEXT_H + GRID_GAP;
      grid->textX    = 0;
      grid->textW    = grid->tileW;
      grid->previewX = grid->previewW = 0;
   }
   grid->visibleRows = grid->height / grid->rowStep;
   if (grid->visibleRows < 1) grid->visibleRows = 1;
}

// (re)create every label at the current text-column width. Labels hold a rasterised texture sized to their
// wrap width, so a view switch has to rebuild them; they come back empty and the window re-materialises them.
static void layoutLabels(VideoGrid *grid)
{
   int titleSize = grid->view == GRID_VIEW_LIST ? LIST_TITLE : ROW_TITLE;
   int metaSize  = grid->view == GRID_VIEW_LIST ? LIST_META  : META_SIZE;
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      freeLabel(&grid->rows[i]);
      freeLabel(&grid->metas[i]);
      freeLabel(&grid->durations[i]);
      initLabel(&grid->rows[i],      grid->font, 0, 0, grid->textW, AUTO, titleSize, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->metas[i],     grid->font, 0, 0, grid->textW, AUTO, metaSize,  activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->durations[i], grid->font, 0, 0, AUTO,        AUTO, DUR_SIZE,  activeTheme->textPrimary,   TEXT_NOWRAP,          "");
   }
   freeLabel(&grid->previewTitle);
   freeLabel(&grid->previewMeta);
   int previewW = grid->previewW > 0 ? grid->previewW : grid->tileW;
   initLabel(&grid->previewTitle, grid->font, 0, 0, previewW, AUTO, PREVIEW_TITLE, activeTheme->textPrimary,   TEXT_WRAP,            "");
   initLabel(&grid->previewMeta,  grid->font, 0, 0, previewW, AUTO, PREVIEW_META,  activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   grid->previewFor = -1;
}

// ---- tile windowing (UI side) ----

static void freeThumbnails(VideoGrid *grid)
{
   finishGfx();
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      if (grid->thumbs[i].jpeg) { free(grid->thumbs[i].jpeg); grid->thumbs[i].jpeg = NULL; }
      freeGfxTexture(&grid->thumbs[i].tex);
   }
   memset(grid->thumbs, 0, sizeof grid->thumbs);
}

static void resetFeed(VideoGrid *grid)
{
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   memset(&grid->results, 0, sizeof grid->results);
   grid->stage = GRID_RUNNING;
   grid->searchParsed = grid->workerDone = 0;
   grid->thumbFetched = 0;
   grid->moreEmpty = grid->moreFailed = 0;
   grid->previewFor = -1;
   grid->page = grid->selected = grid->scrollRow = grid->winStart = grid->winEnd = 0;
}

static void beginFeed(VideoGrid *grid)
{
   if (grid->job == JOB_CACHE) {   // results already in hand: show immediately, worker only fetches thumbnails
      grid->stage = GRID_READY;
      __sync_synchronize();
      grid->searchParsed = 1;
   }
   if (!spawnWorker(grid)) {
      grid->workerDone = 1;
      if (grid->job != JOB_CACHE) { grid->stage = GRID_EMPTY; grid->searchParsed = 1; }
   }
}

// (re)load the current source: restore pendingCached results instantly if set (worker then only refetches
// thumbnails), else fetch page 1 fresh. run either immediately (idle) or deferred after the worker is reaped.
static void applyDeferredSource(VideoGrid *grid)
{
   resetFeed(grid);
   if (grid->pendingCached) { grid->results = *grid->pendingCached; grid->job = JOB_CACHE; }
   else                       grid->job = JOB_LOAD;
   beginFeed(grid);
}

static void loadMore(VideoGrid *grid)
{
   grid->job = JOB_MORE;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// fetch the next band of thumbnails the window has scrolled toward (lazy, non-blocking)
static void loadThumbnails(VideoGrid *grid)
{
   grid->job = JOB_THUMBS;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// join the non-empty metadata parts with " - " into "author - views - age".
static void composeMeta(char *out, int cap, const SearchResult *item)
{
   const char *parts[3] = { item->author, item->views, item->published };
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < 3; i++) {
      if (!parts[i][0]) continue;
      length += snprintf(out + length, cap - length, "%s%s", length ? " \xe2\x80\xa2 " : "", parts[i]);
      if (length >= cap) break;
   }
}

static void materializeTile(VideoGrid *grid, int i)
{
   char meta[128];
   const SearchResult *item = &grid->results.items[i];
   setLabelText(&grid->rows[i], item->title);
   composeMeta(meta, sizeof meta, item);
   setLabelText(&grid->metas[i], meta);
   setLabelText(&grid->durations[i], item->isLive ? "LIVE" : item->duration);
}

static void dematerializeTile(VideoGrid *grid, int i)
{
   freeLabel(&grid->rows[i]);
   freeLabel(&grid->metas[i]);
   freeLabel(&grid->durations[i]);
   if (grid->thumbs[i].tex.offset) { freeGfxTexture(&grid->thumbs[i].tex); memset(&grid->thumbs[i].tex, 0, sizeof grid->thumbs[i].tex); }
}

// keep exactly the visible band (+ margin) materialised. safe without an RSX flush: a tile leaving the
// MARGIN_ROWS-wide window wasn't drawn last frame (the grid draws only the inner visible rows).
static void reconcileWindow(VideoGrid *grid)
{
   int start = (grid->scrollRow - MARGIN_ROWS) * grid->cols;
   int end   = (grid->scrollRow + grid->visibleRows + MARGIN_ROWS) * grid->cols;
   if (start < 0) start = 0;
   if (end > grid->results.count) end = grid->results.count;
   if (start == grid->winStart && end == grid->winEnd) return;

   int lo = start < grid->winStart ? start : grid->winStart;
   int hi = end   > grid->winEnd   ? end   : grid->winEnd;
   for (int i = lo; i < hi; i++) {
      int wasIn = (i >= grid->winStart && i < grid->winEnd);
      int nowIn = (i >= start && i < end);
      if      (nowIn && !wasIn) materializeTile(grid, i);
      else if (wasIn && !nowIn) dematerializeTile(grid, i);
   }
   grid->winStart = start;
   grid->winEnd   = end;
}

static void decodeWindowThumbnails(VideoGrid *grid)
{
   int done = 0;
   for (int i = grid->winStart; i < grid->winEnd && done < DECODES_PER_FRAME; i++)
      if (grid->thumbs[i].state == THUMB_FETCHED && grid->thumbs[i].jpeg && !grid->thumbs[i].tex.offset) {
         grid->thumbs[i].tex = loadGfxTextureMem(grid->thumbs[i].jpeg, (uint32_t)grid->thumbs[i].jpegLen);
         done++;
      }
}

// ---- paging ----
//
// Results are presented a page at a time rather than as one long scroll: a page is exactly what fits on
// screen, and turning one replaces the whole screenful. That makes a position meaningful ("page 3 of 12")
// where a scroll offset is not, and it is why scrollRow is never free - it is always page * visibleRows.

static int pageSize(const VideoGrid *grid) { return grid->visibleRows * grid->cols; }

static int pageCount(const VideoGrid *grid)
{
   int size = pageSize(grid);
   if (size <= 0 || grid->results.count <= 0) return 1;
   return (grid->results.count + size - 1) / size;
}

// move the viewport to a page without touching the selection (callers place it themselves)
static void setPage(VideoGrid *grid, int page)
{
   int last = pageCount(grid) - 1;
   if (page < 0) page = 0;
   if (page > last) page = last;
   grid->page = page;
   grid->scrollRow = page * grid->visibleRows;
}

// R2/L2: turn the page and land on its first result
static void turnPage(VideoGrid *grid, int delta)
{
   if (grid->results.count <= 0) return;   // a restored-but-empty feed is still READY
   int target = grid->page + delta;
   if (target < 0 || target > pageCount(grid) - 1) return;
   setPage(grid, target);
   grid->selected = grid->page * pageSize(grid);
}

// the selection is confined to the current page. A vertical step off the top or bottom edge turns the page
// and keeps the column, which is how a paged list is expected to behave; a horizontal step just stops.
static void moveSelection(VideoGrid *grid, int delta)
{
   if (grid->results.count <= 0) return;
   int size  = pageSize(grid);
   int first = grid->page * size;
   int last  = first + size - 1;
   if (last >= grid->results.count) last = grid->results.count - 1;

   int target = grid->selected + delta;
   if (target >= first && target <= last) { grid->selected = target; return; }
   if (delta != grid->cols && delta != -grid->cols) return;   // horizontal: stop at the edge

   int column = (grid->selected - first) % grid->cols;
   if (delta > 0) {
      if (grid->page + 1 > pageCount(grid) - 1) return;
      setPage(grid, grid->page + 1);
      grid->selected = grid->page * size + column;                                    // top row, same column
   } else {
      if (grid->page == 0) return;
      setPage(grid, grid->page - 1);
      grid->selected = grid->page * size + (grid->visibleRows - 1) * grid->cols + column;   // bottom row
   }
   if (grid->selected >= grid->results.count) grid->selected = grid->results.count - 1;
   if (grid->selected < grid->page * size)    grid->selected = grid->page * size;
}

// ---- public interface ----

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height)
{
   memset(grid, 0, sizeof *grid);
   grid->font = font;
   grid->x = x; grid->y = y; grid->width = width; grid->height = height;
   grid->view = GRID_VIEW_GRID;

   computeGeometry(grid);
   layoutLabels(grid);
   initLabel(&grid->watchLaterBadge, font, 0, 0, AUTO, AUTO, WATCHLATER_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Watch Later");
}

void termVideoGrid(VideoGrid *grid)
{
   stopVideoGrid(grid);
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   freeLabel(&grid->previewTitle);
   freeLabel(&grid->previewMeta);
   freeLabel(&grid->watchLaterBadge);
}

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId)) { grid->isWatched = isWatched; }
void setGridWatchLaterPredicate(VideoGrid *grid, int (*isWatchLater)(const char *videoId)) { grid->isWatchLater = isWatchLater; }
void setGridProgressSource(VideoGrid *grid, int (*watchedPosition)(const char *videoId)) { grid->watchedPosition = watchedPosition; }

void setGridInputSuppressed(VideoGrid *grid, int suppressed)
{
   suppressed = suppressed ? 1 : 0;
   if (suppressed == grid->inputSuppressed) return;
   grid->inputSuppressed = suppressed;
   if (!suppressed) {
      // A held direction must not carry over from whatever had focus - but ZEROING the timers achieves the
      // opposite. isRepeatDue compares `now - timer` against the hold delay, and `now` is microseconds since
      // boot, so timer == 0 reads as "last fired an eternity ago" and the very next frame fires immediately
      // and then auto-repeats. Stamping them with the current time restores the full initial delay, which is
      // what "just arrived here" should mean.
      uint64_t now = sys_time_get_system_time();
      grid->horizontalRepeat.timer = now; grid->horizontalRepeat.repeats = 0;
      grid->verticalRepeat.timer   = now; grid->verticalRepeat.repeats   = 0;
      grid->pageRepeat.timer       = now; grid->pageRepeat.repeats       = 0;
   }
}

GridView gridView(const VideoGrid *grid) { return grid->view; }

void setGridView(VideoGrid *grid, GridView view)
{
   if (view == grid->view) return;

   // drop the materialised window explicitly (rather than just forgetting it) so the thumbnail textures it
   // holds are released - the new geometry re-decodes them from the kept jpegs.
   finishGfx();
   for (int i = grid->winStart; i < grid->winEnd; i++) dematerializeTile(grid, i);
   grid->winStart = grid->winEnd = 0;

   grid->view = view;
   computeGeometry(grid);
   layoutLabels(grid);

   // keep the selection; which page it falls on depends on the new page size
   setPage(grid, grid->selected / pageSize(grid));
}

// both source-changes are non-blocking: if a load is in flight, ask it to stop and defer the swap until the
// reap in updateVideoGrid, so mode / category switches never freeze the frame on an in-flight fetch.
static void changeSource(VideoGrid *grid, GridFetchFn fetch, void *user, const SearchResults *cached)
{
   grid->fetch = fetch;
   grid->fetchUser = user;
   grid->pendingCached = cached;
   if (!grid->threadActive) { applyDeferredSource(grid); return; }
   grid->stopWorker = 1;
   grid->reloadPending = 1;
   grid->stage = GRID_RUNNING;
}

void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, NULL); }
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, cached); }

void stopVideoGrid(VideoGrid *grid)
{
   if (grid->threadActive) { grid->stopWorker = 1; joinThread(grid->workerTid); grid->threadActive = 0; }
   grid->reloadPending = 0;
}

// list view only: keep the right-hand pane showing whatever is selected.
static void refreshPreview(VideoGrid *grid)
{
   if (grid->view != GRID_VIEW_LIST || grid->previewFor == grid->selected || grid->results.count == 0) return;
   const SearchResult *item = &grid->results.items[grid->selected];
   char meta[128];
   composeMeta(meta, sizeof meta, item);
   setLabelText(&grid->previewTitle, item->title);
   setLabelText(&grid->previewMeta, meta);
   grid->previewFor = grid->selected;
}

void updateVideoGrid(VideoGrid *grid)
{
   // reap a finished worker; a deferred source swap then resets and starts the new feed
   if (grid->workerDone && grid->threadActive) {
      joinThread(grid->workerTid); grid->threadActive = 0;
      if (grid->reloadPending) { grid->reloadPending = 0; applyDeferredSource(grid); }
   }
   if (grid->stage != GRID_READY) return;

   reconcileWindow(grid);
   decodeWindowThumbnails(grid);

   if (!grid->inputSuppressed) {
      if      (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveSelection(grid, 1);
      else if (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_LEFT)))  moveSelection(grid, -1);
      if      (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_DOWN)))  moveSelection(grid, grid->cols);
      else if (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_UP)))    moveSelection(grid, -grid->cols);
      // R2/L2 turn the page outright, without having to walk the selection to the edge first
      if      (isRepeatDue(&grid->pageRepeat, getPadButtonState(PAD_BTN_R2))) turnPage(grid,  1);
      else if (isRepeatDue(&grid->pageRepeat, getPadButtonState(PAD_BTN_L2))) turnPage(grid, -1);
   }

   refreshPreview(grid);

   // idle background work: thumbnails for what is on screen, then the next page of results. The fetch runs a
   // page early, so turning to the last page usually finds the one after it already waiting.
   if (!grid->threadActive && !grid->reloadPending) {
      if (grid->thumbFetched < thumbFrontier(grid)) {
         loadThumbnails(grid);
      } else if (grid->results.continuation[0] && grid->results.count < MAX_SEARCH_RESULTS) {
         if (grid->page >= pageCount(grid) - 2) loadMore(grid);
      }
   }
}

// shared badge painting for both views: duration / LIVE bottom-right of the thumbnail, watch-later top-right,
// and the progress line along the very bottom edge.
static void drawBadges(VideoGrid *grid, int i, int tx, int ty, int thumbW)
{
   const SearchResult *item = &grid->results.items[i];

   // how far you got last time. The app has always stored this to resume playback; it was simply never
   // shown, so a half-watched video looked exactly like an untouched one. Sits below the duration badge
   // (which ends 6px up), so the two never overlap.
   if (grid->watchedPosition && !item->isLive) {
      int total = durationToSeconds(item->duration);
      int position = grid->watchedPosition(item->videoId);
      if (total > 0 && position > 0) {
         int filled = position >= total ? thumbW : (int)((long long)thumbW * position / total);
         int barY = ty + grid->thumbH - PROGRESS_BAR_H;
         fillGfxRectangle(tx, barY, thumbW, PROGRESS_BAR_H, activeTheme->badgeFill);
         if (filled > 0) fillGfxRectangle(tx, barY, filled, PROGRESS_BAR_H, activeTheme->accent);
      }
   }
   if (item->duration[0] || item->isLive) {
      int badgeW = grid->durations[i].tt.tex.w + 12, badgeH = DUR_SIZE + 8;
      int badgeX = tx + thumbW - badgeW - 6, badgeY = ty + grid->thumbH - badgeH - 6;
      fillGfxRectangle(badgeX, badgeY, badgeW, badgeH, item->isLive ? activeTheme->accent : activeTheme->badgeFill);
      drawLabelAt(&grid->durations[i], badgeX + 6, badgeY + 4);
   }
   if (grid->isWatchLater && grid->isWatchLater(item->videoId)) {
      int badgeW = grid->watchLaterBadge.tt.tex.w + 12, badgeH = WATCHLATER_BADGE_SIZE + 8;
      int badgeX = tx + thumbW - badgeW - 6, badgeY = ty + 6;
      fillGfxRectangle(badgeX, badgeY, badgeW, badgeH, activeTheme->badgeFill);
      drawLabelAt(&grid->watchLaterBadge, badgeX + 6, badgeY + 4);
   }
}

static void drawThumb(VideoGrid *grid, int i, int tx, int ty, int w, int h, int watched)
{
   if (grid->thumbs[i].tex.offset)
      drawGfxTexture(tx, ty, w, h, grid->thumbs[i].tex, 0, 0, 1, 1, watched ? activeTheme->watchedThumbTint : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangle(tx, ty, w, h, activeTheme->surface);
}

// grid view: thumbnail with the title and meta line underneath.
static void drawTile(VideoGrid *grid, int i)
{
   int row = i / grid->cols, col = i % grid->cols;
   int tx = grid->x + col * (grid->tileW + GRID_GAP);
   int ty = grid->y + (row - grid->scrollRow) * grid->rowStep;
   int watched = grid->isWatched && grid->isWatched(grid->results.items[i].videoId);

   if (i == grid->selected) {
      // Two rings, not one: the bright border, and a wider translucent one outside it. The renderer has no
      // blur, so this layering is what makes the selection read as a glow rather than a flat outline.
      int ring = activeTheme->focusThickness, halo = ring * 3;
      fillGfxRectangle(tx - halo, ty - halo, grid->tileW + 2 * halo, grid->thumbH + 2 * halo, activeTheme->focusGlow);
      fillGfxRectangle(tx - ring, ty - ring, grid->tileW + 2 * ring, grid->thumbH + 2 * ring, activeTheme->focusBorder);
   }
   drawThumb(grid, i, tx, ty, grid->tileW, grid->thumbH, watched);
   drawBadges(grid, i, tx, ty, grid->tileW);

   int textY = ty + grid->thumbH + 6, alpha = watched ? WATCHED_ALPHA : 255;
   moveLabel(&grid->rows[i],  tx, textY);                   drawLabelAlpha(&grid->rows[i],  alpha);
   moveLabel(&grid->metas[i], tx, textY + ROW_TITLE + 8);   drawLabelAlpha(&grid->metas[i], alpha);
}

// list view: one result per row, thumbnail on the left and the text beside it, the way a browser lists them.
static void drawListRow(VideoGrid *grid, int i)
{
   int tx = grid->x;
   int ty = grid->y + (i - grid->scrollRow) * grid->rowStep;
   int watched = grid->isWatched && grid->isWatched(grid->results.items[i].videoId);

   if (i == grid->selected) {
      int ring = activeTheme->focusThickness;
      // the ring spans thumbnail + text, i.e. the whole results column (textX + textW)
      // The ring goes round the thumbnail only, exactly as in the grid view. Filling the whole row with
      // focusBorder instead painted an opaque block under the text column - and focusBorder is white in the
      // stock theme, so the selected row's title (also white) became invisible. The text band gets the
      // translucent rowHighlight instead, which marks the row without hiding it.
      int halo = ring * 3;
      fillGfxRectangle(tx - halo, ty - halo, grid->tileW + 2 * halo, grid->thumbH + 2 * halo, activeTheme->focusGlow);
      fillGfxRectangle(tx - ring, ty - ring, grid->tileW + 2 * ring, grid->thumbH + 2 * ring, activeTheme->focusBorder);
      fillGfxRectangle(tx + grid->textX - LIST_TEXT_GAP / 2, ty - ring,
                       grid->textW + LIST_TEXT_GAP, grid->thumbH + 2 * ring, activeTheme->rowHighlight);
   }
   drawThumb(grid, i, tx, ty, grid->tileW, grid->thumbH, watched);
   drawBadges(grid, i, tx, ty, grid->tileW);

   // the text block sits vertically centred against the thumbnail
   int alpha = watched ? WATCHED_ALPHA : 255;
   int blockH = grid->rows[i].tt.tex.h + 8 + grid->metas[i].tt.tex.h;
   int textY  = ty + (grid->thumbH - blockH) / 2;
   if (textY < ty) textY = ty;
   moveLabel(&grid->rows[i],  tx + grid->textX, textY);                                    drawLabelAlpha(&grid->rows[i],  alpha);
   moveLabel(&grid->metas[i], tx + grid->textX, textY + grid->rows[i].tt.tex.h + 8);       drawLabelAlpha(&grid->metas[i], alpha);
}

// list view: the selection shown large on the right, so the small row thumbnails stay readable.
static void drawPreview(VideoGrid *grid)
{
   if (grid->previewW <= 0 || grid->results.count == 0) return;
   int i = grid->selected;
   int w = grid->previewW, h = w * 9 / 16;
   int x = grid->previewX, y = grid->y;

   drawThumb(grid, i, x, y, w, h, 0);

   int textY = y + h + 16;
   moveLabel(&grid->previewTitle, x, textY);
   drawLabel(&grid->previewTitle);
   moveLabel(&grid->previewMeta, x, textY + grid->previewTitle.tt.tex.h + 10);
   drawLabel(&grid->previewMeta);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;
   for (int i = 0; i < grid->results.count; i++) {
      int row = i / grid->cols;
      if (row < grid->scrollRow || row >= grid->scrollRow + grid->visibleRows) continue;
      if (grid->view == GRID_VIEW_LIST) drawListRow(grid, i);
      else                              drawTile(grid, i);
   }
   if (grid->view == GRID_VIEW_LIST) drawPreview(grid);
}

int                  gridPageIndex(const VideoGrid *grid)    { return grid->page; }
int                  gridPageCount(const VideoGrid *grid)    { return pageCount(grid); }
int                  gridHasMorePages(const VideoGrid *grid) { return grid->results.continuation[0] != 0; }

void formatGridPage(const VideoGrid *grid, char *out, int cap)
{
   // youtube's feeds are continuation-based and never state a total, so the count is what is loaded so far;
   // the trailing + says it is still a running figure rather than the end of the results.
   snprintf(out, cap, "Page %d / %d%s", grid->page + 1, pageCount(grid), gridHasMorePages(grid) ? "+" : "");
}

GridStage            gridStage(const VideoGrid *grid)    { return grid->stage; }
int                  gridBusy(const VideoGrid *grid)     { return grid->threadActive && grid->job == JOB_MORE; }
int                  gridStopRequested(const VideoGrid *grid) { return grid->stopWorker; }
const SearchResults *gridResults(const VideoGrid *grid)  { return &grid->results; }
const SearchResult  *gridSelected(const VideoGrid *grid)
{
   return grid->results.count ? &grid->results.items[grid->selected] : NULL;
}
int                  gridSelectedIndex(const VideoGrid *grid) { return grid->selected; }

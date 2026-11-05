// video-grid - the reusable windowed video-result list (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again when the page turns away from it and re-decoded from the kept jpeg on the
// way back, so VRAM stays flat no matter how many results are loaded. The worker is stopped before playback
// so no per-call http fetch overlaps the streaming http client.
//
// The look is Ember's (UEBERGABE 3.4): every thumbnail is drawn as image, progress line, rounded corners
// (masks in the page colour), then the focus ring when selected, then the badges; the focus glow goes down
// first, under all of them. Titles and meta lines are rendered white once and tinted per frame, so a tile can
// change colour with its state (selected, watched) without being rasterised again.

#include "video-grid.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "http.h"
#include "anim.h"               // interpolateColor: the page colour under the focus glow
#include "ui-fonts.h"           // label helpers (raw text, Manrope or FONT_POP), metrics, type scale
#include "ui-draw.h"            // corners, focus glow + ring, badges, progress bar
#include "dbg.h"                // paging diagnostics
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_GAP    22
// grid view: the text under a thumbnail - title and meta line, one title line each (Bild 4's card with the
// avatar gone): 16 down to the text, the title's line box 28.6, 4, the meta's 27
#define TILE_TEXT_GAP   16
#define TILE_TITLE_BASE 22      // the title's baseline, from the top of the text
#define TILE_META_BASE  54      // the meta line's baseline, likewise
#define TILE_TEXT_H     76      // space under each thumbnail for the title + meta line
#define ROW_TITLE   UI_CARD_TITLE_SIZE    // 22 in Bold
#define META_SIZE   UI_META_SIZE          // 20 in SemiBold
#define TITLE_WEIGHT UI_CARD_TITLE_WEIGHT
#define META_WEIGHT  UI_META_WEIGHT
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
#define LIST_TEXT_GAP   28      // the focus ring takes 9 of it and the selected row's band starts 5 after that
#define LIST_ROW_GAP    14
#define LIST_TITLE      24      // card titles are 22-24 (UEBERGABE 3.3): the larger one, beside a thumbnail
#define LIST_META       UI_META_SIZE
#define LIST_LINE_GAP   18      // a row's title baseline -> its meta line's capitals
#define PREVIEW_TITLE   UI_SHELF_TITLE_SIZE     // the selection shown large: a panel title, 32/800
#define PREVIEW_TITLE_WEIGHT UI_SHELF_TITLE_WEIGHT
#define PREVIEW_META    UI_META_SIZE
#define PREVIEW_TITLE_BASE 46   // the preview image's bottom -> the title's first baseline
#define PREVIEW_META_GAP   18   // the title's last baseline -> the meta line's capitals

#define PROGRESS_BAR_H 6        // "you got this far" line along the bottom edge of a thumbnail
#define BADGE_INSET    10       // badges sit this far in from the thumbnail's edges (Bild 4)
#define RING_OUTSET    9        // UI_RING_THUMB's outer edge lies this far outside the thumbnail (ui-draw.h)
// The glow texture's own alpha at a card's edge (84 of 255, measured on schein-r23): the page under a focused
// thumbnail's corners is appBg lit by that share of focusGlow (times focusGlow's alpha).
#define GLOW_EDGE_SHARE 0.33f
// Titles and meta lines are rendered in white and given their colour when drawn (drawTinted): white times
// the tint is exactly the tint, and the state colours then cost no re-render.
#define TEXT_TINTABLE 0xFFFFFFFFu

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
//
// Everything here shows text from YouTube (a duration is too), so every label is a raw text label: a '{' in a
// title is a character, not markup, and the font is picked per text when it is set - Manrope, or FONT_POP
// for a title Manrope cannot draw (ui-fonts.h). Titles and meta lines are white, see TEXT_TINTABLE.
static void layoutLabels(VideoGrid *grid)
{
   int titleSize = grid->view == GRID_VIEW_LIST ? LIST_TITLE : ROW_TITLE;
   int metaSize  = grid->view == GRID_VIEW_LIST ? LIST_META  : META_SIZE;
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      freeLabel(&grid->rows[i]);
      freeLabel(&grid->metas[i]);
      freeLabel(&grid->durations[i]);
      uiInitTextLabel(&grid->rows[i],      TITLE_WEIGHT,    0, 0, grid->textW, titleSize,     TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->metas[i],     META_WEIGHT,     0, 0, grid->textW, metaSize,      TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->durations[i], UI_BADGE_WEIGHT, 0, 0, AUTO,        UI_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP,          "");
   }
   freeLabel(&grid->previewTitle);
   freeLabel(&grid->previewMeta);
   int previewW = grid->previewW > 0 ? grid->previewW : grid->tileW;
   uiInitTextLabel(&grid->previewTitle, PREVIEW_TITLE_WEIGHT, 0, 0, previewW, PREVIEW_TITLE, activeTheme->textPrimary,   TEXT_WRAP,            "");
   uiInitTextLabel(&grid->previewMeta,  META_WEIGHT,          0, 0, previewW, PREVIEW_META,  activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
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

// join the non-empty metadata parts with a middle dot (U+00B7, as in Ember's cards) into
// "author . views . age". Both Manrope and FONT_POP have the dot.
static void composeMeta(char *out, int cap, const SearchResult *item)
{
   const char *parts[3] = { item->author, item->views, item->published };
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < 3; i++) {
      if (!parts[i][0]) continue;
      length += snprintf(out + length, cap - length, "%s%s", length ? " \xc2\xb7 " : "", parts[i]);
      if (length >= cap) break;
   }
}

// uiSetTextLabel picks each text's font afresh: a dematerialised tile's labels were freed (text emptied), so
// they always render here, in Manrope or - for a title with characters Manrope lacks - FONT_POP.
static void materializeTile(VideoGrid *grid, int i)
{
   char meta[128];
   const SearchResult *item = &grid->results.items[i];
   uiSetTextLabel(&grid->rows[i], TITLE_WEIGHT, item->title);
   composeMeta(meta, sizeof meta, item);
   uiSetTextLabel(&grid->metas[i], META_WEIGHT, meta);
   uiSetTextLabel(&grid->durations[i], UI_BADGE_WEIGHT, item->isLive ? "LIVE" : item->duration);
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
   uiInitLabel(&grid->watchLaterBadge, UI_BADGE_WEIGHT, 0, 0, AUTO, UI_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Watch Later");
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

// A held direction must not carry over from whatever had focus - but ZEROING the timers achieves the
// opposite. isRepeatDue compares `now - timer` against the hold delay, and `now` is microseconds since boot,
// so timer == 0 reads as "last fired an eternity ago" and the very next frame fires immediately and then
// auto-repeats; a timer left from before another screen came up is just as overdue. Stamping them with the
// current time restores the full initial delay, which is what "just arrived here" should mean.
void resetGridRepeat(VideoGrid *grid)
{
   uint64_t now = sys_time_get_system_time();
   grid->horizontalRepeat.timer = now; grid->horizontalRepeat.repeats = 0;
   grid->verticalRepeat.timer   = now; grid->verticalRepeat.repeats   = 0;
   grid->pageRepeat.timer       = now; grid->pageRepeat.repeats       = 0;
}

void setGridInputSuppressed(VideoGrid *grid, int suppressed)
{
   suppressed = suppressed ? 1 : 0;
   if (suppressed == grid->inputSuppressed) return;
   grid->inputSuppressed = suppressed;
   if (!suppressed) resetGridRepeat(grid);   // nothing held over from the overlay may fire on return
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
   uiSetTextLabel(&grid->previewTitle, PREVIEW_TITLE_WEIGHT, item->title);
   uiSetTextLabel(&grid->previewMeta, META_WEIGHT, meta);
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

// ---- drawing ----

// A label rendered in TEXT_TINTABLE, drawn at (x, y) in `color` (an opaque theme colour). Like uiDrawLabelAt
// it leaves the label's own x/y alone.
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

static int isTileWatched(const VideoGrid *grid, int i)
{
   return grid->isWatched && grid->isWatched(grid->results.items[i].videoId);
}

// A tile's text colours. A watched title steps down to textSecondary, as on Ember's watched card - it used to
// be drawn at a quarter of its alpha, which on the Ember palette came out darker than anything else on the
// page. The meta line is textTertiary at rest (it is 20 px, the least that colour is allowed at) and
// textSecondary on the selection, as on Ember's cards and rows.
static uint32_t titleColor(int watched)
{
   return watched ? activeTheme->textSecondary : activeTheme->textPrimary;
}

static uint32_t metaColor(const VideoGrid *grid, int i)
{
   return i == grid->selected ? activeTheme->textSecondary : activeTheme->textTertiary;
}

// top-left of tile i's thumbnail on the current page - one formula for both views, list view being a grid
// of one column
static void tilePosition(const VideoGrid *grid, int i, int *x, int *y)
{
   int row = i / grid->cols, col = i % grid->cols;
   *x = grid->x + col * (grid->tileW + GRID_GAP);
   *y = grid->y + (row - grid->scrollRow) * grid->rowStep;
}

// The page under a focused thumbnail's corners: appBg with the glow's light on it. The glow lies under the
// card, so corner masks in plain appBg would cut four dark notches between the image and the ring.
static uint32_t focusedCornerColor(void)
{
   uint32_t glow = activeTheme->focusGlow;
   float strength = (float)(glow >> 24) / 255.0f * GLOW_EDGE_SHARE;
   return interpolateColor(activeTheme->appBg, uiWithAlpha(glow, 255), strength);
}

// How far you got last time, along the very bottom of the thumbnail. The app has always stored this to
// resume playback; it was simply never shown, so a half-watched video looked exactly like an untouched one.
// It goes on before the corner masks, which then round its ends together with the image.
static void drawProgress(VideoGrid *grid, int i, int tx, int ty, int thumbW)
{
   const SearchResult *item = &grid->results.items[i];
   if (!grid->watchedPosition || item->isLive) return;
   int total = durationToSeconds(item->duration);
   int position = grid->watchedPosition(item->videoId);
   if (total <= 0 || position <= 0) return;
   float fraction = position >= total ? 1.0f : (float)position / (float)total;
   uiDrawProgressBar(tx, ty + grid->thumbH - PROGRESS_BAR_H, thumbW, PROGRESS_BAR_H, fraction,
                     activeTheme->progressTrack, activeTheme->accent, 0);
}

// shared badge painting for both views: duration / LIVE bottom-right of the thumbnail, watch-later top-right.
// BADGE_INSET keeps the bottom one clear of the progress line, so the two never overlap.
static void drawBadges(VideoGrid *grid, int i, int tx, int ty, int thumbW)
{
   const SearchResult *item = &grid->results.items[i];
   const Label *duration = &grid->durations[i];
   // LIVE wears the live colour - in the duration's place for now; it moves top-left with the grid redesign
   if ((item->duration[0] || item->isLive) && duration->tt.tex.w > 0) {
      int badgeX = tx + thumbW - BADGE_INSET - uiGetBadgeWidth(duration);
      int badgeY = ty + grid->thumbH - BADGE_INSET - UI_BADGE_H;
      uiDrawBadge(badgeX, badgeY, duration, item->isLive ? activeTheme->live : activeTheme->badgeFill);
   }
   if (grid->isWatchLater && grid->isWatchLater(item->videoId)) {
      int badgeX = tx + thumbW - BADGE_INSET - uiGetBadgeWidth(&grid->watchLaterBadge);
      uiDrawBadge(badgeX, ty + BADGE_INSET, &grid->watchLaterBadge, activeTheme->badgeFill);
   }
}

static void drawThumb(VideoGrid *grid, int i, int tx, int ty, int w, int h, int watched)
{
   if (grid->thumbs[i].tex.offset)
      drawGfxTexture(tx, ty, w, h, grid->thumbs[i].tex, 0, 0, 1, 1, watched ? activeTheme->watchedThumbTint : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangle(tx, ty, w, h, activeTheme->surface);
}

// A tile's thumbnail, in paint order: the image (or the placeholder until it lands), the progress line,
// rounded corners over both, the focus ring when selected, the badges on top. The renderer has no blur, so
// the selection's glow is a texture - laid under every tile by drawVideoGrid, not here.
static void drawThumbnail(VideoGrid *grid, int i, int tx, int ty, int watched)
{
   int selected = (i == grid->selected);
   drawThumb(grid, i, tx, ty, grid->tileW, grid->thumbH, watched);
   drawProgress(grid, i, tx, ty, grid->tileW);
   uiDrawImageCorners(tx, ty, grid->tileW, grid->thumbH, UI_RADIUS_THUMB,
                      selected ? focusedCornerColor() : activeTheme->appBg);
   if (selected) uiDrawFocusRing(tx, ty, grid->tileW, grid->thumbH, UI_RING_THUMB, activeTheme->focusBorder);
   drawBadges(grid, i, tx, ty, grid->tileW);
}

// grid view: thumbnail with the title and meta line underneath, each placed by its baseline, so a FONT_POP
// title (one Manrope cannot draw) sits on the same line as its Manrope neighbours.
static void drawTile(VideoGrid *grid, int i)
{
   int tx, ty;
   tilePosition(grid, i, &tx, &ty);
   int watched = isTileWatched(grid, i);
   drawThumbnail(grid, i, tx, ty, watched);

   int textTop = ty + grid->thumbH + TILE_TEXT_GAP;
   drawTinted(&grid->rows[i],  tx, uiLabelYForBaseline(&grid->rows[i],  textTop + TILE_TITLE_BASE), titleColor(watched));
   drawTinted(&grid->metas[i], tx, uiLabelYForBaseline(&grid->metas[i], textTop + TILE_META_BASE),  metaColor(grid, i));
}

// list view: one result per row, thumbnail on the left and the text beside it, the way a browser lists them.
static void drawListRow(VideoGrid *grid, int i)
{
   int tx, ty;
   tilePosition(grid, i, &tx, &ty);
   int watched = isTileWatched(grid, i);

   if (i == grid->selected) {
      // The ring goes round the thumbnail only, exactly as in the grid view. Filling the whole row with
      // focusBorder instead painted an opaque block under the text column - and focusBorder can be white
      // (YouTube Classic), so the selected row's title (also white) became invisible. The text band gets
      // the translucent rowHighlight instead, which marks the row without hiding it. It is as tall as the
      // ring and starts just clear of it.
      uiFillRounded(tx + grid->textX - LIST_TEXT_GAP / 2, ty - RING_OUTSET, grid->textW + LIST_TEXT_GAP,
                    grid->thumbH + 2 * RING_OUTSET, UI_RADIUS_ROW, activeTheme->rowHighlight);
   }
   drawThumbnail(grid, i, tx, ty, watched);

   // The text block sits vertically centred against the thumbnail: from the title's capitals to the meta
   // line's baseline, measured per font - a FONT_POP texture puts its glyphs at other rows than Manrope's.
   UiTextMetrics title = uiGetTextMetrics(grid->rows[i].font,  grid->rows[i].size);
   UiTextMetrics meta  = uiGetTextMetrics(grid->metas[i].font, grid->metas[i].size);
   int titleCaps = title.baseline - title.capTop, metaCaps = meta.baseline - meta.capTop;
   int titleBase = ty + (grid->thumbH - (titleCaps + LIST_LINE_GAP + metaCaps)) / 2 + titleCaps;
   int metaBase  = titleBase + LIST_LINE_GAP + metaCaps;
   drawTinted(&grid->rows[i],  tx + grid->textX, titleBase - title.baseline, titleColor(watched));
   drawTinted(&grid->metas[i], tx + grid->textX, metaBase - meta.baseline,   metaColor(grid, i));
}

// list view: the selection shown large on the right, so the small row thumbnails stay readable.
static void drawPreview(VideoGrid *grid)
{
   if (grid->previewW <= 0 || grid->results.count == 0) return;
   int i = grid->selected;
   int w = grid->previewW, h = w * 9 / 16;
   int x = grid->previewX, y = grid->y;

   drawThumb(grid, i, x, y, w, h, 0);
   uiDrawImageCorners(x, y, w, h, UI_RADIUS_CARD, activeTheme->appBg);

   // The title wraps over as many lines as it needs; the meta line goes under the last one. A wrapped
   // texture is one line's texture plus one line pitch per further line, which gives that line's baseline.
   int titleY = uiLabelYForBaseline(&grid->previewTitle, y + h + PREVIEW_TITLE_BASE);
   uiDrawLabelAt(&grid->previewTitle, x, titleY);
   UiTextMetrics title = uiGetTextMetrics(grid->previewTitle.font, grid->previewTitle.size);
   UiTextMetrics meta  = uiGetTextMetrics(grid->previewMeta.font,  grid->previewMeta.size);
   int lastBase = titleY + grid->previewTitle.tt.tex.h - title.texH + title.baseline;
   int metaBase = lastBase + PREVIEW_META_GAP + (meta.baseline - meta.capTop);
   uiDrawLabelAt(&grid->previewMeta, x, metaBase - meta.baseline);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;
   int first = grid->scrollRow * grid->cols;
   int end   = (grid->scrollRow + grid->visibleRows) * grid->cols;
   if (end > grid->results.count) end = grid->results.count;

   // The selection's glow goes down before ANY tile: it spreads 81 px, further than the gap to the
   // neighbours, so drawn with its own tile it would lie over the tiles before it and under those after it.
   if (grid->selected >= first && grid->selected < end) {
      int tx, ty;
      tilePosition(grid, grid->selected, &tx, &ty);
      uiDrawFocusGlow(tx, ty, grid->tileW, grid->thumbH, activeTheme->focusGlow);
   }
   for (int i = first; i < end; i++) {
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

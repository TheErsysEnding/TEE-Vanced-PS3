// video-grid - the reusable windowed video-result list (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again when the page turns away from it and re-decoded from the kept jpeg on the
// way back, so VRAM stays flat no matter how many results are loaded. The worker is stopped before playback
// so no per-call http fetch overlaps the streaming http client.
//
// The look is Ember's (UEBERGABE 3.4; the cards of Bild 4, the rows of Bild 5): every thumbnail is drawn as
// image, progress line, rounded corners (masks in the colour behind it), then the focus ring when selected,
// then the badges; the focus glow goes down first, under all of them. Titles and meta lines are rendered
// white once and tinted per frame, so a tile can change colour with its state (selected, watched) without
// being rasterised again.

#include "video-grid.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "http.h"
#include "anim.h"               // interpolateColor / applyEasing: the glow under a card, the focus easing
#include "ui-fonts.h"           // label helpers (raw text, Manrope or FONT_POP), metrics, type scale
#include "ui-draw.h"            // corners, focus glow + ring, badges, progress bar
#include "dbg.h"                // paging diagnostics
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---- grid view: the list pages' cards (Bild 4) ----
#define GRID_COL_GAP      32
#define GRID_ROW_GAP      44      // a card's meta line -> the next row's thumbnails
// A card's text starts 16 under its thumbnail: a title of up to two lines in 1.3-em line boxes, then, 4
// lower, the meta line (Bild 4's card with the avatar gone). The title is 24/700, the top of the design's
// 22-24 for card titles: these cards are 388 px wide where Bild 4's are 342. The baselines are where the
// design's line boxes put them, counted from the top of the text.
#define TILE_TEXT_GAP     16
#define TILE_TITLE_SIZE   24
#define TILE_TITLE_BASE   25      // (31.2 - 33) / 2 + 26: the first line's baseline in its line box
#define TILE_TITLE_LINE   31      // 24 x 1.3: the second line's baseline is this much lower
#define TILE_META_GAP     4
#define TILE_TEXT_H       93      // two title lines, the gap and the meta line's 27-px box
// The focused card grows to 104 % about its centre and its text moves 6 down, clear of the bigger picture
// (Bild 4); the change takes about 150 ms, EASE_OUT_CUBIC (UEBERGABE 3.4). Only the look is eased - the
// selection itself moves at once, so input never waits for an animation.
#define FOCUS_GROW        0.02f   // a side
#define FOCUS_TEXT_SHIFT  6
#define FOCUS_EASE_US     150000
#define FOCUS_TITLE_WEIGHT UI_WEIGHT_EXTRABOLD   // Bild 4 sets the focused card's title in 800, the others in 700

// ---- list view: the search results' rows (Bild 5) ----
#define LIST_ROW_GAP      20
#define LIST_PAD          18      // a row's padding round its thumbnail
#define LIST_TEXT_GAP     32      // thumbnail -> text column
#define LIST_RADIUS       26      // the focused row's plate: the radius UI_RING_ROW is made for
#define LIST_THUMB_RADIUS 16
// The row title is 28/700 focused or not. Bild 5 sets the focused one in 30/800, but a size or weight change
// means rasterising two titles on every move of the selection; the plate, ring and glow mark the row.
#define LIST_TITLE_SIZE   28
#define LIST_TITLE_BASE   28      // (33.6 - 38) / 2 + 30: the first line's baseline in its 1.2-em line box
#define LIST_TITLE_LINE   34      // 28 x 1.2
#define LIST_META_GAP     12      // the title's last line box -> the meta line's box
#define LIST_BADGE_BOTTOM 16      // Bild 5 lifts the duration badge this far up when a progress line runs under it

// ---- both views ----
#define TITLE_WEIGHT      UI_CARD_TITLE_WEIGHT   // 700
#define META_SIZE         UI_META_SIZE           // 20
#define META_WEIGHT       UI_META_WEIGHT         // 600
#define CHANNEL_WEIGHT    UI_WEIGHT_BOLD         // list view: the channel, a step heavier than the rest of its line
#define META_ASCENT       21      // a 20-px line box: the baseline 21 below its top ...
#define META_BOX          27      // ... in a box 27 tall
#define MARGIN_ROWS 2           // rows kept materialised above and below the visible band
#define THUMB_LOOKAHEAD_ROWS 3  // rows below the visible band to fetch thumbnails for ahead of scrolling
#define DECODES_PER_FRAME 3
#define MATERIALIZE_PER_FRAME 2 // tiles outside the visible page whose labels are rasterised per frame
#define THUMB_CAP   (96 * 1024) // one mqdefault jpeg fits easily
#define THUMB_UA    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

#define PROGRESS_BAR_H 6        // "you got this far" line along the bottom edge of a thumbnail
#define BADGE_INSET    10       // badges sit this far in from the thumbnail's edges (Bild 4)
#define BADGE_SPACE    8        // two badges along one edge keep at least this far apart, or the left one goes
// The glow texture's own alpha at a card's edge (84 of 255, measured on schein-r23): the page under a focused
// thumbnail's corners is appBg lit by that share of focusGlow (times focusGlow's alpha).
#define GLOW_EDGE_SHARE 0.33f
// A text is drawn without the library's ellipsis only when it fits with this much to spare: that mode also
// cuts a text which just fits (it reserves room for the three dots), and a renderer that rounds its pen
// differently from the measurement must not lose the last letter instead.
#define FIT_SLACK      2
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
      // Exactly LIST_ROWS rows fill the rectangle: each takes a third of its height, less the gap under it,
      // and its thumbnail is as tall as the row's padding leaves - so the rows scale with what the screen
      // can spare, keeping Bild 5's proportions.
      grid->cols        = 1;
      grid->visibleRows = LIST_ROWS;
      grid->rowStep     = grid->height / LIST_ROWS;
      grid->rowH        = grid->rowStep - LIST_ROW_GAP;
      grid->thumbH      = grid->rowH - 2 * LIST_PAD;
      if (grid->thumbH < 1) grid->thumbH = 1;
      grid->tileW       = grid->thumbH * 16 / 9;
      grid->textX       = LIST_PAD + grid->tileW + LIST_TEXT_GAP;
      grid->textW       = grid->width - grid->textX - LIST_PAD;
   } else {
      grid->cols        = GRID_COLS;
      grid->tileW       = (grid->width - (GRID_COLS - 1) * GRID_COL_GAP) / GRID_COLS;
      grid->thumbH      = grid->tileW * 9 / 16;
      grid->rowH        = grid->thumbH + TILE_TEXT_GAP + TILE_TEXT_H;
      grid->rowStep     = grid->rowH + GRID_ROW_GAP;
      grid->textX       = 0;
      grid->textW       = grid->tileW;
      // whole rows only: the last one needs no gap under it
      grid->visibleRows = (grid->height + GRID_ROW_GAP) / grid->rowStep;
   }
   if (grid->textW < 1) grid->textW = 1;
   if (grid->visibleRows < 1) grid->visibleRows = 1;
}

// (re)create every label at the current text-column width. Labels hold a rasterised texture sized to their
// wrap width, so a view switch has to rebuild them; they come back empty and the window re-materialises them.
//
// Everything here shows text from YouTube (a duration is too), so every label is a raw text label: a '{' in a
// title is a character, not markup, and the font is picked per text when it is set - Manrope, or FONT_POP
// for a title Manrope cannot draw (ui-fonts.h). Titles and meta lines are white, see TEXT_TINTABLE.
static void freeTileLabels(VideoGrid *grid, int i)
{
   freeLabel(&grid->titles[i]);
   freeLabel(&grid->titles2[i]);
   freeLabel(&grid->channels[i]);
   freeLabel(&grid->metas[i]);
   freeLabel(&grid->durations[i]);
}

// the focused card's own title pair (labelFocusTitle): emptied, so the next selection rasterises it afresh
static void freeFocusTitle(VideoGrid *grid)
{
   freeLabel(&grid->focusTitle);
   freeLabel(&grid->focusTitle2);
   grid->focusTitleIndex = -1;
}

static void layoutLabels(VideoGrid *grid)
{
   int titleSize = grid->view == GRID_VIEW_LIST ? LIST_TITLE_SIZE : TILE_TITLE_SIZE;
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      freeTileLabels(grid, i);
      uiInitTextLabel(&grid->titles[i],    TITLE_WEIGHT,    0, 0, grid->textW, titleSize,     TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->titles2[i],   TITLE_WEIGHT,    0, 0, grid->textW, titleSize,     TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->channels[i],  CHANNEL_WEIGHT,  0, 0, grid->textW, META_SIZE,     TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->metas[i],     META_WEIGHT,     0, 0, grid->textW, META_SIZE,     TEXT_TINTABLE,            TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&grid->durations[i], UI_BADGE_WEIGHT, 0, 0, AUTO,        UI_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP,          "");
   }
   freeFocusTitle(grid);
   uiInitTextLabel(&grid->focusTitle,  FOCUS_TITLE_WEIGHT, 0, 0, grid->textW, titleSize, TEXT_TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitTextLabel(&grid->focusTitle2, FOCUS_TITLE_WEIGHT, 0, 0, grid->textW, titleSize, TEXT_TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
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
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) freeTileLabels(grid, i);
   freeFocusTitle(grid);
   memset(grid->labelled, 0, sizeof grid->labelled);
   memset(&grid->results, 0, sizeof grid->results);
   grid->stage = GRID_RUNNING;
   grid->searchParsed = grid->workerDone = 0;
   grid->thumbFetched = 0;
   grid->moreEmpty = grid->moreFailed = 0;
   grid->page = grid->selected = grid->scrollRow = grid->winStart = grid->winEnd = 0;
   grid->focusIndex = grid->focusFrom = -1;   // a new feed: no card to shrink back from
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

// join the non-empty parts with a middle dot (U+00B7, as in Ember's cards and rows): "author . views . age".
// afterChannel starts the line with a dot too, for the list view's meta, which follows its channel label.
// Both Manrope and FONT_POP have the dot.
static void joinMeta(char *out, int cap, const char *const *parts, int count, int afterChannel)
{
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < count; i++) {
      if (!parts[i][0]) continue;
      int separate = length > 0 || afterChannel;
      length += snprintf(out + length, cap - length, "%s%s", separate ? " \xc2\xb7 " : "", parts[i]);
      if (length >= cap) break;
   }
}

// One character, decoded as the library's renderer decodes it (font.c): a malformed byte is a character of
// its own. Only its advance and whether it is a space matter to the line break below.
static uint32_t nextChar(const char *text, int *at)
{
   const unsigned char *p = (const unsigned char *)text + *at;
   uint32_t c = p[0];
   int length = 1;
   if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
      length = 2;
   } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      c = ((c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      length = 3;
   } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      c = ((c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      length = 4;
   }
   *at += length;
   return c;
}

// How much of a title goes on its first line at `width` px, measured with the advances the renderer will use
// (the same font and size): as many whole words as fit. A first word wider than the line - or a title with
// no spaces at all, as Japanese ones are - is cut at the last character that fits instead. Returns the byte
// count of the first line; *next is where the second starts, past the spaces at the break. The library can
// wrap, but not stop after two lines nor end the second with "...", hence one label per line.
static int fitFirstLine(Font *font, int size, const char *text, int width, int *next)
{
   int at = 0, breakAt = -1;
   float pen = 0.0f;
   // measureFontChar does not check that the font is open (measureFontText does)
   if (!font->open) { *next = (int)strlen(text); return *next; }
   while (text[at]) {
      int start = at;
      uint32_t code = nextChar(text, &at);
      if (code == ' ' && start > 0) breakAt = start;
      pen += measureFontChar(font, size, code);
      if (pen > (float)width) {
         int end = breakAt > 0 ? breakAt : (start > 0 ? start : at);
         int rest = end;
         while (text[rest] == ' ') rest++;
         *next = rest;
         return end;
      }
   }
   *next = at;
   return at;
}

// Sets EXTERNAL text in a font the caller picked, clipped at `width` px: drawn whole where it fits, ending in
// "..." where it does not. A label renders with the font, width and wrap mode it holds, and setLabelText skips
// a text the label already shows, so a change to any of them drops the old texture first.
static void setFittedText(Label *l, Font *font, int width, const char *text)
{
   if (!text) text = "";
   if (width < 1) width = 1;
   TextWrap wrap = measureFontText(font, l->size, text) <= (float)(width - FIT_SLACK) ? TEXT_NOWRAP : TEXT_NOWRAP_ELLIPSIS;
   if (l->font != font || !l->raw || l->width != width || l->wrap != wrap) {
      freeLabel(l);
      l->font  = font;
      l->raw   = 1;
      l->width = width;
      l->wrap  = wrap;
   }
   setLabelText(l, text);
}

// What a tile shows as its title and as its channel, both cleaned (uiCleanText): an emoji no font has would be
// a gap an em wide, and one at a title's line break began its second line with a blank indent. Neither font
// has a letter of Thai, Arabic or Hindi, though, and a title in such a script would still be nothing but such
// gaps round a stray "|" or digit - a card that looks broken (uiTextIsLegible). The channel name takes the
// title's place then, the one other name a result has, and leaves the meta line so that it does not show
// twice. A channel name that would come out blank is left out wherever it would go, so neither line is ever a
// row of gaps: with both unreadable the card shows its views and age alone.
static void tileTexts(const SearchResult *item, char *title, int titleCap, char *author, int authorCap)
{
   uiCleanText(author, authorCap, item->author);
   if (!uiTextIsLegible(author)) author[0] = '\0';
   uiCleanText(title, titleCap, item->title);
   if (uiTextIsLegible(title)) return;
   strCopy(title, titleCap, author);
   author[0] = '\0';
}

// Puts a title (as tileTexts made it) on its two labels at `weight`: on the first as many whole words as fit
// (fitFirstLine), the rest on the second. Both lines are in the font picked for the WHOLE title, so a title
// Manrope cannot draw is in FONT_POP throughout rather than changing font at the break.
static void setTitleLines(const VideoGrid *grid, Label *first, Label *second, const char *title, UiWeight weight)
{
   Font *font = uiPickFont(weight, title);
   char line[sizeof grid->results.items[0].title];
   int next = 0;
   int length = fitFirstLine(font, first->size, title, grid->textW - FIT_SLACK, &next);
   if (length >= (int)sizeof line) length = (int)sizeof line - 1;
   memcpy(line, title, (size_t)length);
   line[length] = '\0';
   setFittedText(first,  font, grid->textW, line);
   setFittedText(second, font, grid->textW, title + next);
}

// Rasterises a tile's labels. A dematerialised tile's labels were freed (text emptied), so everything renders
// here.
static void materializeTile(VideoGrid *grid, int i)
{
   const SearchResult *item = &grid->results.items[i];
   grid->labelled[i] = 1;
   char title[sizeof item->title], author[sizeof item->author];
   tileTexts(item, title, (int)sizeof title, author, (int)sizeof author);
   setTitleLines(grid, &grid->titles[i], &grid->titles2[i], title, TITLE_WEIGHT);

   // the channel name and the meta line are external text too: cleaned like the title before a font is
   // picked for them (a channel named with an emoji would otherwise leave its gap and pull in FONT_POP)
   const char *rest[2] = { item->views, item->published };
   char meta[160];
   if (grid->view == GRID_VIEW_LIST) {
      // Bild 5's meta line: the channel in its own label, so it can be the brighter part of the line, and the
      // rest right after it. The channel gets at most three fifths of the column - a long name is cut rather
      // than squeezing out the views and the age.
      setFittedText(&grid->channels[i], uiPickFont(CHANNEL_WEIGHT, author), grid->textW * 3 / 5, author);
      joinMeta(meta, sizeof meta, rest, 2, author[0] != 0);
      uiCleanText(meta, (int)sizeof meta, meta);
      setFittedText(&grid->metas[i], uiPickFont(META_WEIGHT, meta), grid->textW - grid->channels[i].tt.tex.w, meta);
   } else {
      const char *parts[3] = { author, item->views, item->published };
      joinMeta(meta, sizeof meta, parts, 3, 0);
      uiCleanText(meta, (int)sizeof meta, meta);
      setFittedText(&grid->metas[i], uiPickFont(META_WEIGHT, meta), grid->textW, meta);
   }
   uiSetTextLabel(&grid->durations[i], UI_BADGE_WEIGHT, item->isLive ? "LIVE" : item->duration);
}

// The thumbnail is retired rather than freed: a page turn in the list view moves the window by three rows,
// past its two-row margin, so a tile leaving it may have been drawn in the frame the RSX is still reading.
static void dematerializeTile(VideoGrid *grid, int i)
{
   freeTileLabels(grid, i);
   grid->labelled[i] = 0;
   if (grid->thumbs[i].tex.offset) { retireGfxTexture(&grid->thumbs[i].tex); memset(&grid->thumbs[i].tex, 0, sizeof grid->thumbs[i].tex); }
}

// keep exactly the visible band (+ margin) materialised; what leaves it is released (see dematerializeTile).
// What enters it is labelled by labelWindow.
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
      if (wasIn && !nowIn) dematerializeTile(grid, i);
   }
   grid->winStart = start;
   grid->winEnd   = end;
}

// Rasterises the labels of the tiles in the window: every one on the visible page at once, the rest - the
// rows either side, kept ready for the next page turn - MATERIALIZE_PER_FRAME a frame, the following page
// first. A card has up to four labels now (two title lines), and a page turn rasterising the whole next page
// in one frame would stall exactly the frame that shows the turn.
static void labelWindow(VideoGrid *grid)
{
   int first = grid->scrollRow * grid->cols;
   int end   = first + grid->visibleRows * grid->cols;
   if (first < grid->winStart) first = grid->winStart;
   if (end > grid->winEnd) end = grid->winEnd;
   for (int i = first; i < end; i++) if (!grid->labelled[i]) materializeTile(grid, i);

   int budget = MATERIALIZE_PER_FRAME;
   for (int i = end; i < grid->winEnd && budget > 0; i++)
      if (!grid->labelled[i]) { materializeTile(grid, i); budget--; }
   for (int i = grid->winStart; i < first && budget > 0; i++)
      if (!grid->labelled[i]) { materializeTile(grid, i); budget--; }
}

// Grid view: the focused card's title in 800 (Bild 4). Re-rendering the tiles' own labels in another weight
// would rasterise two cards' titles on every move of the selection, and again on the way back; instead the
// title the selection lands on is rasterised once more, into a pair of labels of its own. The card's 700
// labels stay as they are for when the focus moves on.
static void labelFocusTitle(VideoGrid *grid)
{
   if (grid->view != GRID_VIEW_GRID || grid->results.count <= 0 || grid->focusTitleIndex == grid->selected) return;
   grid->focusTitleIndex = grid->selected;
   const SearchResult *item = &grid->results.items[grid->selected];
   char title[sizeof item->title], author[sizeof item->author];
   tileTexts(item, title, (int)sizeof title, author, (int)sizeof author);   // the same title the card has
   setTitleLines(grid, &grid->focusTitle, &grid->focusTitle2, title, FOCUS_TITLE_WEIGHT);
}

// Decodes kept jpegs for the window, DECODES_PER_FRAME a frame - the visible page's first, so after a jump
// the pictures on screen arrive before those of the pages either side.
static int decodeThumbnail(VideoGrid *grid, int i)
{
   if (grid->thumbs[i].state != THUMB_FETCHED || !grid->thumbs[i].jpeg || grid->thumbs[i].tex.offset) return 0;
   grid->thumbs[i].tex = loadGfxTextureMem(grid->thumbs[i].jpeg, (uint32_t)grid->thumbs[i].jpegLen);
   return 1;
}

static void decodeWindowThumbnails(VideoGrid *grid)
{
   int first = grid->scrollRow * grid->cols;
   int end   = first + grid->visibleRows * grid->cols;
   if (first < grid->winStart) first = grid->winStart;
   if (end > grid->winEnd) end = grid->winEnd;
   int done = 0;
   for (int i = first; i < end && done < DECODES_PER_FRAME; i++) done += decodeThumbnail(grid, i);
   for (int i = grid->winStart; i < grid->winEnd && done < DECODES_PER_FRAME; i++) done += decodeThumbnail(grid, i);
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
   int size   = pageSize(grid);
   int first  = grid->page * size;
   int last   = first + size - 1;
   int column = (grid->selected - first) % grid->cols;
   if (last >= grid->results.count) last = grid->results.count - 1;

   // In the grid view LEFT and RIGHT keep to the selection's row. The leftmost column is where LEFT belongs
   // to the rail (home.c opens it on that press): held there, LEFT stops instead of wrapping to the end of the
   // row above, so the next press reaches the rail rather than a card three columns away. RIGHT stops at the
   // rightmost column to match: wrapped to the first card of the next row, the LEFT meant to take it back
   // would open the rail instead.
   if (grid->cols > 1 && delta == -1 && column == 0)              return;
   if (grid->cols > 1 && delta ==  1 && column == grid->cols - 1) return;

   int target = grid->selected + delta;
   if (target >= first && target <= last) { grid->selected = target; return; }
   if (delta != grid->cols && delta != -grid->cols) return;   // horizontal: stop at the edge

   // DOWN onto the page's last row where it is shorter than the row above (the end of a feed) lands on that
   // row's last card, as a step onto a shorter next page does below: from a column past that card DOWN would
   // otherwise do nothing, with cards in plain view under it. Only off the bottom row does DOWN turn the page.
   if (delta > 0 && target < first + size && target - column <= last) { grid->selected = last; return; }

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
   grid->focusIndex = grid->focusFrom = -1;

   computeGeometry(grid);
   layoutLabels(grid);
   uiInitLabel(&grid->watchLaterBadge, UI_BADGE_WEIGHT, 0, 0, AUTO, UI_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Watch Later");
   uiInitLabel(&grid->watchedBadge, UI_BADGE_WEIGHT, 0, 0, AUTO, UI_BADGE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "WATCHED");
}

void termVideoGrid(VideoGrid *grid)
{
   stopVideoGrid(grid);
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) freeTileLabels(grid, i);
   freeFocusTitle(grid);
   freeLabel(&grid->watchLaterBadge);
   freeLabel(&grid->watchedBadge);
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

void setGridFocusHidden(VideoGrid *grid, int hidden) { grid->focusHidden = hidden ? 1 : 0; }

// item i drawn as the focused one: the selection, unless the focus is elsewhere on the screen
static int showsFocus(const VideoGrid *grid, int i) { return i == grid->selected && !grid->focusHidden; }

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

void updateVideoGrid(VideoGrid *grid)
{
   // reap a finished worker; a deferred source swap then resets and starts the new feed
   if (grid->workerDone && grid->threadActive) {
      joinThread(grid->workerTid); grid->threadActive = 0;
      if (grid->reloadPending) { grid->reloadPending = 0; applyDeferredSource(grid); }
   }
   if (grid->stage != GRID_READY) return;

   if (!grid->inputSuppressed) {
      if      (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveSelection(grid, 1);
      else if (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_LEFT)))  moveSelection(grid, -1);
      if      (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_DOWN)))  moveSelection(grid, grid->cols);
      else if (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_UP)))    moveSelection(grid, -grid->cols);
      // R2/L2 turn the page outright, without having to walk the selection to the edge first
      if      (isRepeatDue(&grid->pageRepeat, getPadButtonState(PAD_BTN_R2))) turnPage(grid,  1);
      else if (isRepeatDue(&grid->pageRepeat, getPadButtonState(PAD_BTN_L2))) turnPage(grid, -1);
   }

   // after the input, so a page turned just now is labelled before the frame that shows it
   reconcileWindow(grid);
   labelWindow(grid);
   labelFocusTitle(grid);
   decodeWindowThumbnails(grid);

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

// What a tile shows of your history. A video you stopped part way is STARTED: its progress line, the picture
// at full strength (Bild 4). One you finished - the player stores 0 for it - or opened without getting
// anywhere is WATCHED: dimmed, with the badge. The app has always stored the position to resume playback; it
// was simply never shown, so a half-watched video used to look exactly like an untouched one.
//
// A live stream is neither, on every page: it cannot be finished, having watched it yesterday says nothing
// about what it broadcasts now, and a channel that is on air around the clock would stay dimmed for good.
typedef struct {
   float progress;   // share of the video watched, 0 = no line to draw
   int   watched;
} TileState;

static TileState tileState(const VideoGrid *grid, int i)
{
   TileState state;
   state.progress = 0.0f;
   state.watched  = 0;
   const SearchResult *item = &grid->results.items[i];
   int total    = durationToSeconds(item->duration);
   int position = (grid->watchedPosition && !item->isLive) ? grid->watchedPosition(item->videoId) : 0;
   if (total > 0 && position > 0) state.progress = position >= total ? 1.0f : (float)position / (float)total;
   else                           state.watched  = !item->isLive && grid->isWatched && grid->isWatched(item->videoId);
   return state;
}

// The colour under a focused thumbnail's corners: appBg lit by the glow beneath the card (`strength` = how far
// the glow has faded in). Corner masks in plain appBg would cut four dark notches between the image and the ring.
static uint32_t cornerColor(float strength)
{
   if (strength <= 0.0f) return activeTheme->appBg;
   uint32_t glow = activeTheme->focusGlow;
   float share = (float)(glow >> 24) / 255.0f * GLOW_EDGE_SHARE * strength;
   return interpolateColor(activeTheme->appBg, uiWithAlpha(glow, 255), share);
}

// A thumbnail in paint order: the picture (or the placeholder until it lands; dimmed once watched), the
// progress line along its bottom, then the corner masks, which round both.
static void drawImage(VideoGrid *grid, int i, int x, int y, int w, int h, int radius, uint32_t behind, TileState state)
{
   if (grid->thumbs[i].tex.offset)
      drawGfxTexture(x, y, w, h, grid->thumbs[i].tex, 0, 0, 1, 1, state.watched ? activeTheme->watchedThumbTint : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangle(x, y, w, h, activeTheme->surface);
   if (state.progress > 0.0f)
      uiDrawProgressBar(x, y + h - PROGRESS_BAR_H, w, PROGRESS_BAR_H, state.progress,
                        activeTheme->progressTrack, activeTheme->accent, 0);
   uiDrawImageCorners(x, y, w, h, radius, behind);
}

// The badges on a thumbnail (x, y, w, h), as Bild 4 places them: LIVE top left in the live colour (a live
// video has no duration), the duration bottom right, WATCHED bottom left, the Watch Later tag top right.
// bottomInset keeps the bottom pair clear of a progress line. A badge that would run into its neighbour on a
// small thumbnail is left out rather than drawn over it - the duration and LIVE win.
static void drawBadges(VideoGrid *grid, int i, int x, int y, int w, int h, int bottomInset, int watched)
{
   const SearchResult *item = &grid->results.items[i];
   const Label *duration = &grid->durations[i];
   int topLeftEnd = x;              // where the badges along each edge end, for the next one to keep clear of
   int bottomRightStart = x + w;
   int bottomY = y + h - bottomInset - UI_BADGE_H;

   if (duration->tt.tex.w > 0 && item->isLive) {
      topLeftEnd = x + BADGE_INSET + uiDrawBadge(x + BADGE_INSET, y + BADGE_INSET, duration, activeTheme->live);
   } else if (duration->tt.tex.w > 0 && item->duration[0]) {
      bottomRightStart = x + w - BADGE_INSET - uiGetBadgeWidth(duration);
      uiDrawBadge(bottomRightStart, bottomY, duration, activeTheme->badgeFill);
   }
   if (watched && x + BADGE_INSET + uiGetBadgeWidth(&grid->watchedBadge) + BADGE_SPACE <= bottomRightStart)
      uiDrawBadge(x + BADGE_INSET, bottomY, &grid->watchedBadge, activeTheme->badgeFill);
   if (grid->isWatchLater && grid->isWatchLater(item->videoId)) {
      int badgeX = x + w - BADGE_INSET - uiGetBadgeWidth(&grid->watchLaterBadge);
      if (badgeX >= topLeftEnd + BADGE_SPACE) uiDrawBadge(badgeX, y + BADGE_INSET, &grid->watchLaterBadge, activeTheme->badgeFill);
   }
}

// the top-left of tile i's box on the current page: a card's thumbnail in grid view, a row in list view
static void itemPosition(const VideoGrid *grid, int i, int *x, int *y)
{
   int row = i / grid->cols, col = i % grid->cols;
   *x = grid->x + col * (grid->tileW + GRID_COL_GAP);
   *y = grid->y + (row - grid->scrollRow) * grid->rowStep;
}

// A card's thumbnail box, grown about its centre by `grow` (0 = at rest, 1 = fully focused).
static void cardBox(const VideoGrid *grid, int i, float grow, int *x, int *y, int *w, int *h)
{
   int growX = (int)((float)grid->tileW  * FOCUS_GROW * grow + 0.5f);
   int growY = (int)((float)grid->thumbH * FOCUS_GROW * grow + 0.5f);
   itemPosition(grid, i, x, y);
   *x -= growX;
   *y -= growY;
   *w = grid->tileW  + 2 * growX;
   *h = grid->thumbH + 2 * growY;
}

// How far the focus look has come for the current selection, 0..1 eased. A change of selection starts it
// again; the card it left shrinks back over the same time (1 - this).
static float focusProgress(VideoGrid *grid)
{
   uint64_t now = sys_time_get_system_time();
   if (grid->selected != grid->focusIndex) {
      grid->focusFrom    = grid->focusIndex;
      grid->focusIndex   = grid->selected;
      grid->focusSinceUs = now;
   }
   uint64_t elapsed = now - grid->focusSinceUs;
   if (elapsed >= FOCUS_EASE_US) return 1.0f;
   return applyEasing((float)elapsed / (float)FOCUS_EASE_US, EASE_OUT_CUBIC);
}

// the glow under card i at `strength` (its alpha follows the card's growth), if the card is on this page
static void drawCardGlow(VideoGrid *grid, int i, float strength, int first, int end)
{
   if (i < first || i >= end || strength <= 0.0f) return;
   int x, y, w, h;
   cardBox(grid, i, strength, &x, &y, &w, &h);
   uint32_t glow = activeTheme->focusGlow;
   uiDrawFocusGlow(x, y, w, h, uiWithAlpha(glow, (int)((float)(glow >> 24) * strength + 0.5f)));
}

// grid view: a card - the thumbnail with its badges, and under it the title (one or two lines) and the meta
// line, each placed by its baseline, so a FONT_POP title (one Manrope cannot draw) sits on the same line its
// Manrope neighbours do.
static void drawCard(VideoGrid *grid, int i, float grow)
{
   int x, y, w, h, tx, ty;
   cardBox(grid, i, grow, &x, &y, &w, &h);
   itemPosition(grid, i, &tx, &ty);
   int selected = showsFocus(grid, i);
   TileState state = tileState(grid, i);

   drawImage(grid, i, x, y, w, h, UI_RADIUS_THUMB, cornerColor(grow), state);
   if (selected) uiDrawFocusRing(x, y, w, h, UI_RING_THUMB, activeTheme->focusBorder);
   drawBadges(grid, i, x, y, w, h, BADGE_INSET, state.watched);

   // The title steps down to text-2 once watched, as on Ember's watched card - it used to be drawn at a
   // quarter of its alpha, which on the Ember palette came out darker than anything else on the page. Focus
   // outranks that: the focused card's title is text-1 in 800 (Bild 4), or a watched card's title would read no
   // brighter than its meta line. The meta line is text-3 at rest (20 px, the least that colour is allowed at)
   // and text-2 on the selection.
   uint32_t titleColor = (selected || !state.watched) ? activeTheme->textPrimary : activeTheme->textSecondary;
   uint32_t metaColor  = selected ? activeTheme->textSecondary : activeTheme->textTertiary;
   // the 800 pair once labelFocusTitle has rasterised it for this card; until then its own 700 labels
   const Label *title  = &grid->titles[i];
   const Label *title2 = &grid->titles2[i];
   if (selected && grid->focusTitleIndex == i) { title = &grid->focusTitle; title2 = &grid->focusTitle2; }
   int textTop = ty + grid->thumbH + TILE_TEXT_GAP + (int)((float)FOCUS_TEXT_SHIFT * grow + 0.5f);
   int lines   = title2->tt.tex.w > 0 ? 2 : 1;
   int base    = textTop + TILE_TITLE_BASE;
   drawTinted(title,  tx, uiLabelYForBaseline(title,  base), titleColor);
   drawTinted(title2, tx, uiLabelYForBaseline(title2, base + TILE_TITLE_LINE), titleColor);
   drawTinted(&grid->metas[i],   tx, uiLabelYForBaseline(&grid->metas[i],
              textTop + lines * TILE_TITLE_LINE + TILE_META_GAP + META_ASCENT), metaColor);
}

// list view: a row - the thumbnail on the left, the title and the meta line beside it, the block of text
// centred on the thumbnail. The selected row lies on a surface1 plate with the ring round it (Bild 5).
static void drawRow(VideoGrid *grid, int i)
{
   int rx, ry;
   itemPosition(grid, i, &rx, &ry);
   int selected = showsFocus(grid, i);
   TileState state = tileState(grid, i);

   if (selected) uiFillRounded(rx, ry, grid->width, grid->rowH, LIST_RADIUS, activeTheme->surface1);
   int tx = rx + LIST_PAD, ty = ry + (grid->rowH - grid->thumbH) / 2;
   drawImage(grid, i, tx, ty, grid->tileW, grid->thumbH, LIST_THUMB_RADIUS,
             selected ? activeTheme->surface1 : activeTheme->appBg, state);
   if (selected) uiDrawFocusRing(rx, ry, grid->width, grid->rowH, UI_RING_ROW, activeTheme->focusBorder);
   drawBadges(grid, i, tx, ty, grid->tileW, grid->thumbH, state.progress > 0.0f ? LIST_BADGE_BOTTOM : BADGE_INSET,
              state.watched);

   int lines = grid->titles2[i].tt.tex.w > 0 ? 2 : 1;
   int top   = ty + (grid->thumbH - (lines * LIST_TITLE_LINE + LIST_META_GAP + META_BOX)) / 2;
   int textX = rx + grid->textX;
   int base  = top + LIST_TITLE_BASE;
   int metaBase = top + lines * LIST_TITLE_LINE + LIST_META_GAP + META_ASCENT;
   // as on a card, focus outranks watched: the selected row's title stays text-1 above its text-1 channel
   uint32_t titleColor = (selected || !state.watched) ? activeTheme->textPrimary : activeTheme->textSecondary;
   drawTinted(&grid->titles[i],  textX, uiLabelYForBaseline(&grid->titles[i],  base), titleColor);
   drawTinted(&grid->titles2[i], textX, uiLabelYForBaseline(&grid->titles2[i], base + LIST_TITLE_LINE), titleColor);
   // the channel a step brighter than the views and the age after it, as Bild 5 has it
   drawTinted(&grid->channels[i], textX, uiLabelYForBaseline(&grid->channels[i], metaBase),
              selected ? activeTheme->textPrimary : activeTheme->textSecondary);
   drawTinted(&grid->metas[i], textX + grid->channels[i].tt.tex.w, uiLabelYForBaseline(&grid->metas[i], metaBase),
              selected ? activeTheme->textSecondary : activeTheme->textTertiary);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;
   int first = grid->scrollRow * grid->cols;
   int end   = (grid->scrollRow + grid->visibleRows) * grid->cols;
   if (end > grid->results.count) end = grid->results.count;
   float focus = focusProgress(grid);

   // The selection's glow goes down before ANY tile: it spreads 81 px, further than the gap to the
   // neighbours, so drawn with its own tile it would lie over the tiles before it and under those after it.
   if (grid->view == GRID_VIEW_LIST) {
      if (!grid->focusHidden && grid->selected >= first && grid->selected < end) {
         int rx, ry;
         itemPosition(grid, grid->selected, &rx, &ry);
         uiDrawFocusGlow(rx, ry, grid->width, grid->rowH, activeTheme->focusGlow);
      }
      for (int i = first; i < end; i++) drawRow(grid, i);
      return;
   }

   if (grid->focusHidden) focus = 0.0f;   // the focus is elsewhere: no card grows or glows
   drawCardGlow(grid, grid->focusFrom, grid->focusHidden ? 0.0f : 1.0f - focus, first, end);
   drawCardGlow(grid, grid->selected, focus, first, end);
   for (int i = first; i < end; i++) {
      float grow = grid->focusHidden ? 0.0f : i == grid->selected ? focus : i == grid->focusFrom ? 1.0f - focus : 0.0f;
      drawCard(grid, i, grow);
   }
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

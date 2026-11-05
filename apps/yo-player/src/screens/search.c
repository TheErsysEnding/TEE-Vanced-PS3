// search screen - a VideoGrid in one of two modes: a text search or a channel's videos (see search.h).
// Pushed on top of home. Triangle drills into a channel in place, Circle backs out (channel -> search
// results -> pop to home), SELECT cycles the sort, Start re-searches, X plays.

#include "screens/search.h"
#include "screens/search-input.h"
#include "screens/playlist-screen.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "downloads.h"
#include "ui-hints.h"           // the Ember hint row
#include "ui-draw.h"            // safe area, rounded panel, focus ring, uiDrawLabelAt
#include "ui-fonts.h"           // the shared fonts, the type scale, label helpers and text placement
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"
#include <string.h>
#include <stdio.h>

// The list starts 9 px in from the safe area's left edge: that is how far the focus ring reaches outside a
// thumbnail (UI_RING_THUMB), so the ring stays inside the safe area too. The title lines up with the list.
#define CONTENT_X         (UI_SAFE_X + 9)
// The header is one line: the title on the left, the sort mode and the page count right-aligned, all three
// with their capitals centred on the band of a top-right hint row (UI_HINTS_TOP_Y, centre y 71) - the line
// home.c puts its title on, so the heading stays where it was when a search opens over Home.
#define HEADER_Y          UI_HINTS_TOP_Y
#define HEADER_H          UI_HINT_GLYPH_H
#define HEADER_GAP        32        // between the title, the sort mode and the page count
#define SORT_SIZE         UI_META_SIZE
#define SORT_WEIGHT       UI_WEIGHT_BOLD      // the sort in force is a value: 20/700 in text-1, as in Bild 5
#define PAGE_SIZE         UI_META_SIZE
#define PAGE_WEIGHT       UI_META_WEIGHT
#define GRID_TOP          140       // clear of the title's descenders and of the focus ring 9 px above a row
#define GRID_BOTTOM       (UI_HINTS_BOTTOM_Y - 16)   // the list's rectangle ends above the hint row; until v0.13
                                                     // it ran to the bottom edge, under the hints
#define STATUS_BASELINE   (GRID_TOP + 30)
#define NOTICE_PAD_X      44        // modal confirmation box: inner padding left and right
#define NOTICE_PAD_TOP    40        //   panel top -> the first capitals of the message
#define NOTICE_PAD_BOTTOM 30        //   the "Press X" line -> panel bottom
#define NOTICE_GAP        24        //   the message -> the "Press X" line
#define NOTICE_TEXT       24
#define NOTICE_HINT_H     UI_HINT_GLYPH_H   // the "Press X" line is centred in a band as tall as a hint row
#define NOTICE_RADIUS     26        // the search rows' radius - the one the row ring (UI_RING_ROW) is made for
#define TRIANGLE_HINT     3         // Triangle's slot in the hint row (see initSearch): retitled per mode

static const char *SortNames[SORT_COUNT]                = { "Relevance", "Views" };
static const char *ChannelSortNames[CHANNEL_SORT_COUNT] = { "Latest", "Popular", "Oldest" };

// how the next push opens (set by openSearchQuery / openChannelFor before pushScreen).
static char         pendingQuery[128];
static SearchResult pendingChannel;
static int          pendingIsChannel;

static struct {
   VideoGrid grid;
   char      query[128];
   char      channelId[32];
   char      channelName[48];
   char      channelSortToken[MAX_SORT_TOKEN];   // first-page token for the chosen channel sort ("" = Latest)
   int       channelSort;
   int       sort;
   int       screenW, screenH;
   int       noticeVisible;      // a modal confirmation is up and owns the pad
   int       noticePanelW;
} search;

static Label   titleLabel, sortLabel, statusLabel, pageLabel;
static Label   noticeLabel, noticeHintLabel;
static UiHints hints;

static int searchFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (search.channelId[0]) {
      const char *first = search.channelSortToken[0] ? search.channelSortToken : NULL;
      return getChannelVideos(search.channelId, token ? token : first, out);
   }
   return searchVideos(search.query, search.sort, token, out);
}

// A modal notice, dismissed with X. The subscribe toggle used to be completely silent, so a refused write,
// an unreadable file or a press that never reached the toggle all looked identical to success: a button that
// visibly did nothing. Every outcome now says which one it was.
static void showNotice(const char *text)
{
   uiSetTextLabel(&noticeLabel, UI_BODY_WEIGHT, text);   // may name a channel id: drawn raw, any font it needs
   search.noticeVisible = 1;
}

// The hint row has to fit between the safe edges, and much of its width is the console's own glyph art, whose
// size is only known at run time. Should the art come out wider than expected, the gaps narrow instead of the
// row running off the left edge; at the expected size the full gap fits with room to spare.
static void fitHintRow(UiHints *row)
{
   int gap = UI_HINT_GAP;
   uiSetHintsGap(row, gap);
   while (uiGetHintsWidth(row) > UI_SAFE_RIGHT - UI_SAFE_X && gap > UI_HINT_GAP / 2) {
      gap -= 2;
      uiSetHintsGap(row, gap);
   }
}

// in channel mode Triangle toggles the subscription (you're already in the channel); elsewhere it drills in.
static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = isSubscribed(search.channelId) ? "Unsubscribe" : "Subscribe";
   uiSetHintCaption(&hints, TRIANGLE_HINT, caption);
   fitHintRow(&hints);   // the row's width follows its captions
}

// title (left) shows the mode; sortLabel (right) shows the current ordering.
static void setHeading(void)
{
   char heading[192];
   if (search.channelId[0]) { snprintf(heading, sizeof heading, "%s", search.channelName); setLabelText(&sortLabel, ChannelSortNames[search.channelSort]); }
   else                     { snprintf(heading, sizeof heading, "Search: %s", search.query); setLabelText(&sortLabel, SortNames[search.sort]); }
   // a channel name or a query: drawn raw, and all of it in FONT_POP when Manrope lacks one of its characters
   uiSetTextLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, heading);
   updateTriangleHint();
}

static void toggleSubscription(void)
{
   int wantOn = !isSubscribed(search.channelId);
   int rc = setSubscribed(search.channelId, search.channelName, wantOn);

   updateTriangleHint();
   if (rc == SUBSCRIBE_OK) return;   // the hint flipping to Unsubscribe IS the confirmation; a box on top of
                                     // a working button is just something else to dismiss

   char message[224];
   switch (rc) {
   case SUBSCRIBE_ERR_FULL:
      snprintf(message, sizeof message, "Subscription list is full - %d channels is the maximum.", MAX_SUBSCRIPTIONS);
      break;
   case SUBSCRIBE_ERR_READ:
      snprintf(message, sizeof message, "Could not read this file, so nothing was changed:\n\n%s",
               getSubscriptionsPath());
      break;
   case SUBSCRIBE_ERR_WRITE:
      // name the actual file: which directory the build is using is the single most useful fact here
      if (!isStorageWritable())
         snprintf(message, sizeof message,
                  "The data folder could not be created or written at startup, so nothing can be saved.\n\n%s",
                  getSubscriptionsPath());
      else
         snprintf(message, sizeof message,
                  "%s - but it could not be saved, so it will be gone after a restart.\n\n%s\n\nFree space: %lu KB",
                  wantOn ? "Subscribed" : "Unsubscribed", getSubscriptionsPath(), getStorageFreeKB());
      break;
   default:
      snprintf(message, sizeof message, "This channel has no usable id, so it cannot be subscribed to. (id: \"%s\")", search.channelId);
      break;
   }
   showNotice(message);
}

static void reload(void)
{
   setHeading();
   setGridSource(&search.grid, searchFetch, NULL);
}

static void enterChannel(const SearchResult *item)
{
   if (!item->channelId[0]) return;
   strCopy(search.channelId, sizeof search.channelId, item->channelId);
   strCopy(search.channelName, sizeof search.channelName, item->author[0] ? item->author : "Channel");
   search.channelSort = CHANNEL_SORT_LATEST;
   search.channelSortToken[0] = 0;
   reload();
}

static void cycleChannelSort(void)
{
   const SearchResults *results = gridResults(&search.grid);
   if (!results->sortTokens[CHANNEL_SORT_LATEST][0]) return;   // chips not loaded yet
   search.channelSort = (search.channelSort + 1) % CHANNEL_SORT_COUNT;
   strCopy(search.channelSortToken, sizeof search.channelSortToken, results->sortTokens[search.channelSort]);
   reload();
}

static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (strstr(text, "youtu")) { stopVideoGrid(&search.grid); playVideo(text); return; }
   search.channelId[0] = 0;   // a search exits channel mode
   strCopy(search.query, sizeof search.query, text);
   search.sort = SORT_RELEVANCE;
   reload();
}

// How wide a label of fixed text will come out: the renderer makes the texture exactly as wide as the
// advances add up to, so measuring them in the label's font and size predicts it - with a few px to spare.
static int measureTextWidth(UiWeight weight, int size, const char *text)
{
   float width = measureFontText(uiGetFont(weight), size, text);   // 0 when no font opened at all
   if (width <= 0.0f) width = (float)strlen(text) * (float)size * 0.6f;
   return (int)width + 4;
}

// The widest the page count can get. There are at most 500 results, so never more than three digits, and
// Manrope's digits are proportional - "Page 111" is narrower than "Page 888" - so the sample uses its widest.
static int measurePageReserve(void)
{
   Font *font = uiGetFont(PAGE_WEIGHT);
   char widest = '0';
   float best = 0.0f;
   if (font->open) {   // measureFontChar does not check that itself
      for (char c = '0'; c <= '9'; c++) {
         float advance = measureFontChar(font, PAGE_SIZE, (uint32_t)(unsigned char)c);
         if (advance > best) { best = advance; widest = c; }
      }
   }
   char sample[32];
   snprintf(sample, sizeof sample, "Page %c%c%c / %c%c%c+", widest, widest, widest, widest, widest, widest);
   return measureTextWidth(PAGE_WEIGHT, PAGE_SIZE, sample);
}

// A label's width is fixed when it is made, and the title's has to leave room for the sort mode and page
// count right of it, which change as you go. It used to be a flat screen width minus 300, so a long query
// ran on underneath them; sized against the widest both can ever be, it now ends in "..." short of them.
static int measureTitleWidth(void)
{
   int sortW = 0;
   for (int i = 0; i < SORT_COUNT; i++) {
      int width = measureTextWidth(SORT_WEIGHT, SORT_SIZE, SortNames[i]);
      if (width > sortW) sortW = width;
   }
   for (int i = 0; i < CHANNEL_SORT_COUNT; i++) {
      int width = measureTextWidth(SORT_WEIGHT, SORT_SIZE, ChannelSortNames[i]);
      if (width > sortW) sortW = width;
   }
   int titleW = UI_SAFE_RIGHT - measurePageReserve() - HEADER_GAP - sortW - HEADER_GAP - CONTENT_X;
   return titleW > 400 ? titleW : 400;
}

static void initSearch(void)
{
   search.screenW = getGfxScreenWidth();
   search.screenH = getGfxScreenHeight();

   memset(search.channelId, 0, sizeof search.channelId);
   search.query[0] = 0;
   search.sort = SORT_RELEVANCE;
   if (pendingIsChannel) {
      strCopy(search.channelId, sizeof search.channelId, pendingChannel.channelId);
      strCopy(search.channelName, sizeof search.channelName, pendingChannel.author[0] ? pendingChannel.author : "Channel");
      search.channelSort = CHANNEL_SORT_LATEST;
      search.channelSortToken[0] = 0;
   } else {
      strCopy(search.query, sizeof search.query, pendingQuery);
   }

   // The fonts are the shared ones (ui-fonts.h) - nothing here opens or closes one. Labels are positioned
   // when they are drawn, by their measured capitals, since the title may come out in FONT_POP.
   uiInitTextLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, measureTitleWidth(), UI_PAGE_TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&sortLabel,   SORT_WEIGHT,   0, 0, AUTO, SORT_SIZE, activeTheme->textPrimary,   TEXT_NOWRAP, "");
   uiInitLabel(&statusLabel, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   uiInitLabel(&pageLabel,   PAGE_WEIGHT,   0, 0, AUTO, PAGE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   search.noticeVisible = 0;
   search.noticePanelW  = search.screenW * 2 / 3;
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, search.noticePanelW - 2 * NOTICE_PAD_X, NOTICE_TEXT, activeTheme->textPrimary, TEXT_WRAP, "");
   uiInitLabel(&noticeHintLabel, UI_HINT_WEIGHT, 0, 0, AUTO, UI_HINT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Press X to close");

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,    "Play");
   uiAddHint(&hints, GLYPH_CIRCLE,   "Back");
   uiAddHint(&hints, GLYPH_SQUARE,   "Watch Later");
   uiAddHint(&hints, GLYPH_TRIANGLE, "Channel");
   uiAddHint(&hints, GLYPH_SELECT,   "Sort");
   uiAddHint(&hints, GLYPH_R3,       "Download");
   uiAddHint(&hints, GLYPH_L3,       "Add to playlist");
   uiAddHint(&hints, GLYPH_START,    "Search");
   uiAddHint(&hints, GLYPH_L2,       "");
   uiAddHint(&hints, GLYPH_R2,       "Page");

   initVideoGrid(&search.grid, uiGetFont(UI_CARD_TITLE_WEIGHT), CONTENT_X, GRID_TOP,
                 UI_SAFE_RIGHT - CONTENT_X, GRID_BOTTOM - GRID_TOP);
   setGridView(&search.grid, GRID_VIEW_LIST);   // the list is the only layout now
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   setGridProgressSource(&search.grid, getWatchedPosition);
   reload();
}

// Back on the results from the player, the search field or the playlist chooser. A direction or L2/R2 still
// held from there - the player's UP/DOWN volume, say - must not move the list the moment it reappears, but
// the grid's repeat timers still hold the time of its last move, so isRepeatDue took the hold for an overdue
// repeat: it fired on the first frame and then raced on at up to 50 steps a second. Stamped with the current
// time, a held button waits out the normal delay before its first repeat, exactly as if it had just been
// pressed here; a fresh press still acts at once.
static void resumeSearch(void)
{
   resetGridRepeat(&search.grid);
}

static void updateSearch(void)
{
   updateDownloadOverlay();
   if (oskInputActive()) return;

   if (search.noticeVisible) {   // modal: nothing underneath sees the pad until it is dismissed
      if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_CIRCLE)) search.noticeVisible = 0;
      return;
   }

   // START opens the app's own search field, prefilled with the current query
   if (isPadButtonPressed(PAD_BTN_START)) { openSearchInput(search.query, onQueryEntered); return; }

   // Circle: channel-with-underlying-search -> back to results; otherwise pop to home
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      if (search.channelId[0] && search.query[0]) { search.channelId[0] = 0; reload(); }
      else popScreen();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_SELECT)) {
      if (search.channelId[0]) cycleChannelSort();
      else { search.sort = (search.sort + 1) % SORT_COUNT; reload(); }
      return;
   }


   // In channel mode Triangle subscribes, and that is about the CHANNEL, not about its videos - so it has to
   // work whatever the feed is doing. Below the GRID_READY gate it was dead while the feed loaded, and dead
   // for good on a channel whose Videos feed comes back empty, while the hint already read "Subscribe".
   if (search.channelId[0] && isPadButtonPressed(PAD_BTN_TRIANGLE)) { toggleSubscription(); return; }

   int pageBefore = gridPageIndex(&search.grid);
   updateVideoGrid(&search.grid);
   // one line per page turn in dbg.txt, as on home: VRAM has to come back to the same level page after page
   if (gridPageIndex(&search.grid) != pageBefore) uiLogVram("search page turn");
   if (gridStage(&search.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&search.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) { toggleWatchLater(selected); return; }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }
   if (isPadButtonPressed(PAD_BTN_L3)) { openPlaylistChooser(selected); return; }   // L3 came free with the grid layout   // live can't be downloaded
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { enterChannel(selected); return; }   // channel mode handled above
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatchedItem(selected);   // full entry, so History can list it
      stopVideoGrid(&search.grid);
      // hand over the whole list so autoplay can carry on with what follows
      playVideoFromList(gridResults(&search.grid), gridSelectedIndex(&search.grid));
   }
}

// title left, then - right-aligned - the sort mode and, once results are in, the page count
static void drawHeader(void)
{
   int rightEdge = UI_SAFE_RIGHT;
   if (gridStage(&search.grid) == GRID_READY) {
      char page[40];
      formatGridPage(&search.grid, page, sizeof page);
      setLabelText(&pageLabel, page);   // only rasterises when the text actually changes
      rightEdge -= pageLabel.tt.tex.w;
      uiDrawLabelAt(&pageLabel, rightEdge, uiLabelYForCenter(&pageLabel, HEADER_Y, HEADER_H));
      rightEdge -= HEADER_GAP;
   }
   // right-aligned, left of the page count
   uiDrawLabelAt(&sortLabel, rightEdge - sortLabel.tt.tex.w, uiLabelYForCenter(&sortLabel, HEADER_Y, HEADER_H));
   uiDrawLabelAt(&titleLabel, CONTENT_X, uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H));
}

// The modal box: the scrim dims everything under it, then a rounded panel with the focus ring round it - the
// box owns the pad until it is dismissed, which is what the orange frame said in the old look too.
static void drawNotice(void)
{
   // the message's texture starts above its first capitals by an amount that depends on its font
   UiTextMetrics metrics = uiGetTextMetrics(noticeLabel.font, noticeLabel.size);
   int textTop = NOTICE_PAD_TOP - metrics.capTop;
   if (textTop < 0) textTop = 0;
   int panelW = search.noticePanelW;
   int panelH = textTop + noticeLabel.tt.tex.h + NOTICE_GAP + NOTICE_HINT_H + NOTICE_PAD_BOTTOM;
   int x = (search.screenW - panelW) / 2, y = (search.screenH - panelH) / 2;
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->scrim);
   uiFillRounded(x, y, panelW, panelH, NOTICE_RADIUS, activeTheme->surface);
   uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);
   uiDrawLabelAt(&noticeLabel, x + NOTICE_PAD_X, y + textTop);
   int hintY = y + textTop + noticeLabel.tt.tex.h + NOTICE_GAP;
   uiDrawLabelAt(&noticeHintLabel, x + NOTICE_PAD_X, uiLabelYForCenter(&noticeHintLabel, hintY, NOTICE_HINT_H));
}

static void drawSearch(void)
{
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->appBg);

   // The list goes down first and the heading over it: the focused row's glow reaches 81 px out from its
   // thumbnail, up into the header, and text drawn before it would come out tinted orange.
   if (gridStage(&search.grid) == GRID_READY) {
      drawVideoGrid(&search.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
      uiDrawLabelAt(&statusLabel, CONTENT_X, uiLabelYForBaseline(&statusLabel, STATUS_BASELINE));
   }
   drawHeader();

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   drawDownloadOverlay(search.screenW);

   if (search.noticeVisible) drawNotice();
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   uiTermHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&sortLabel);
   freeLabel(&statusLabel);
   freeLabel(&pageLabel);
   freeLabel(&noticeLabel);
   freeLabel(&noticeHintLabel);
   // no closeFont: the fonts are shared, and closing one would blank every label on every screen
}

void openSearchQuery(const char *query)
{
   strCopy(pendingQuery, sizeof pendingQuery, query);
   pendingIsChannel = 0;
   pushScreen(&searchScreen);
}

void openChannelFor(const SearchResult *item)
{
   // without an id there is no channel to browse; pushing anyway lands on a screen that is in neither mode
   // (headed "Search: " with no query) and whose Triangle hint lies about what the button does.
   if (!item->channelId[0]) return;
   pendingChannel = *item;
   pendingIsChannel = 1;
   pushScreen(&searchScreen);
}

Screen searchScreen = { initSearch, resumeSearch, updateSearch, drawSearch, NULL, termSearch, SCREEN_TERMINATED };

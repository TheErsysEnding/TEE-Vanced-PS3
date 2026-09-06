// search screen - a VideoGrid in one of two modes: a text search or a channel's videos (see search.h).
// Pushed on top of home. Triangle drills into a channel in place, Circle backs out (channel -> search
// results -> pop to home), SELECT cycles the sort, Start re-searches, X plays.

#include "screens/search.h"
#include "screens/search-input.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "downloads.h"
#include "ui/button-hints.h"
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

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define GRID_TOP      74
#define HINTS_BOTTOM  42
#define HINT_GLYPH_H  25
#define HINT_TEXT     21
#define NOTICE_PAD    28        // modal confirmation box: inner padding
#define NOTICE_TEXT   26
#define NOTICE_HINT   20
#define HEADER_GAP    28        // between the sort mode and the page count in the header
#define TRIANGLE_HINT 3         // Triangle's slot in the hint row (see initSearch): retitled per mode

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

static Font        font;
static Label       titleLabel, sortLabel, statusLabel, pageLabel;
static Label       noticeLabel, noticeHintLabel;
static ButtonHints hints;

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
   setLabelText(&noticeLabel, text);
   search.noticeVisible = 1;
}

// in channel mode Triangle toggles the subscription (you're already in the channel); elsewhere it drills in.
static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = isSubscribed(search.channelId) ? "Unsubscribe" : "Subscribe";
   setButtonHintCaption(&hints, TRIANGLE_HINT, caption);
}

// title (left) shows the mode; sortLabel (right) shows the current ordering.
static void setHeading(void)
{
   char heading[192];
   if (search.channelId[0]) { snprintf(heading, sizeof heading, "%s", search.channelName); setLabelText(&sortLabel, ChannelSortNames[search.channelSort]); }
   else                     { snprintf(heading, sizeof heading, "Search: %s", search.query); setLabelText(&sortLabel, SortNames[search.sort]); }
   setLabelText(&titleLabel, heading);
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

   font = openSystemFont(FONT_POP);
   initLabel(&titleLabel,  &font, MARGIN_X, HEADER_Y, search.screenW - 300, AUTO, TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&sortLabel,   &font, 0, HEADER_Y, AUTO, AUTO, TITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   initLabel(&statusLabel, &font, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   initLabel(&pageLabel,   &font, 0, HEADER_Y, AUTO, AUTO, TITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   search.noticeVisible = 0;
   search.noticePanelW  = search.screenW * 2 / 3;
   initLabelRaw(&noticeLabel,     &font, 0, 0, search.noticePanelW - 2 * NOTICE_PAD, AUTO, NOTICE_TEXT, activeTheme->textPrimary,   TEXT_WRAP,   "");
   initLabelRaw(&noticeHintLabel, &font, 0, 0, AUTO, AUTO, NOTICE_HINT, activeTheme->textSecondary, TEXT_NOWRAP, "Press X to close");

   initButtonHints(&hints, &font, search.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE),   "Back");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT),   "Sort");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R3),       "Download");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Search");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L2),       "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R2),       "Page");

   initVideoGrid(&search.grid, &font, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, search.screenH - GRID_TOP);
   setGridView(&search.grid, GRID_VIEW_LIST);   // the list is the only layout now
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   setGridProgressSource(&search.grid, getWatchedPosition);
   reload();
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

   updateVideoGrid(&search.grid);
   if (gridStage(&search.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&search.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) { toggleWatchLater(selected); return; }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { enterChannel(selected); return; }   // channel mode handled above
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatchedItem(selected);   // full entry, so History can list it
      stopVideoGrid(&search.grid);
      // hand over the whole list so autoplay can carry on with what follows
      playVideoFromList(gridResults(&search.grid), gridSelectedIndex(&search.grid));
   }
}

static void drawSearch(void)
{
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);

   int rightEdge = search.screenW - MARGIN_X;
   if (gridStage(&search.grid) == GRID_READY) {
      char page[40];
      formatGridPage(&search.grid, page, sizeof page);
      setLabelText(&pageLabel, page);   // only rasterises when the text actually changes
      moveLabel(&pageLabel, rightEdge - pageLabel.tt.tex.w, HEADER_Y);
      drawLabel(&pageLabel);
      rightEdge -= pageLabel.tt.tex.w + HEADER_GAP;
   }
   moveLabel(&sortLabel, rightEdge - sortLabel.tt.tex.w, HEADER_Y);   // right-aligned, left of the page count
   drawLabel(&sortLabel);

   if (gridStage(&search.grid) == GRID_READY) {
      drawVideoGrid(&search.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }

   drawButtonHints(&hints, search.screenW);
   drawDownloadOverlay(search.screenW);

   if (search.noticeVisible) {
      int panelW = search.noticePanelW;
      int panelH = noticeLabel.tt.tex.h + 2 * NOTICE_PAD + NOTICE_HINT + 18;
      int x = (search.screenW - panelW) / 2, y = (search.screenH - panelH) / 2;
      int ring = activeTheme->focusThickness;
      fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->scrim);
      fillGfxRectangle(x - ring, y - ring, panelW + 2 * ring, panelH + 2 * ring, activeTheme->focusBorder);
      fillGfxRectangle(x, y, panelW, panelH, activeTheme->surface);
      drawLabelAt(&noticeLabel, x + NOTICE_PAD, y + NOTICE_PAD);
      drawLabelAt(&noticeHintLabel, x + NOTICE_PAD, y + panelH - NOTICE_PAD - NOTICE_HINT + 6);
   }
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&sortLabel);
   freeLabel(&statusLabel);
   freeLabel(&pageLabel);
   freeLabel(&noticeLabel);
   freeLabel(&noticeHintLabel);
   closeFont(&font);
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

Screen searchScreen = { initSearch, NULL, updateSearch, drawSearch, NULL, termSearch, SCREEN_TERMINATED };

// home - the landing screen: trending categories in a VideoGrid (see home.h).

#include "screens/home.h"
#include "screens/search.h"
#include "screens/search-input.h"
#include "screens/settings-screen.h"
#include "sidebar.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "downloads.h"
#include "ps-button.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define GRID_TOP      74
#define GRID_LEFT     (SIDEBAR_WIDTH + 26)
// The grid's height used to run to the bottom of the screen and simply happened not to reach the hint row.
// That is not a property anyone should rely on: the row count is derived from the thumbnail size, which is
// derived from the width, so any future change to either can silently push a row into the hints. Measured,
// today's layout still fits five rows with room to spare - this reserve is a guard, not a fix.
#define HINTS_RESERVE 44
#define HINTS_BOTTOM  42        // hint-row top offset from the screen bottom (overlaid, no layout shift)
#define HINT_GLYPH_H  25
#define HINT_TEXT     21
#define CROSS_HINT    0         // X's slot in the hint row (see initHome): retitled per category
#define TRIANGLE_HINT 2         // Triangle's slot: "Channel" everywhere, "Unsubscribe" in Subscriptions
#define NOTICE_PAD    28        // modal box: same shape as the one in search.c
#define NOTICE_TEXT   26
#define NOTICE_HINT   20

static const char *TrendingNames[TREND_COUNT] = { "Gaming", "Sports", "Podcasts" };

// home categories = the trending feeds plus a Subscriptions feed and the Watch Later queue.
#define HOME_SUBS        TREND_COUNT
#define HOME_CONTINUE    (TREND_COUNT + 1)
#define HOME_WATCHLATER  (TREND_COUNT + 2)
#define HOME_HISTORY     (TREND_COUNT + 3)
#define HOME_CAT_COUNT   (TREND_COUNT + 4)

// each category's fully-loaded results, so returning to a tab skips the feed fetch (thumbnails still refetch).
static SearchResults categoryCache[HOME_CAT_COUNT];
static int           categoryCached[HOME_CAT_COUNT];
static int           subsRevisionSeen;         // subscriptions revision the cached HOME_SUBS feed was built at
static int           watchLaterRevisionSeen;   // watch-later revision the cached HOME_WATCHLATER feed was built at
static int           historyRevisionSeen;      // watch-history revision the cached HOME_HISTORY feed was built at

static struct {
   VideoGrid grid;
   int       category;
   int       screenW, screenH;
   int       noticeVisible;                    // a modal box is up and owns the pad
   int       noticeIsConfirm;                  // X carries out the pending unsubscribe (else it just closes)
   int       noticePanelW;
   int       pendingReload;                    // a stale feed to reload on the next update, not on resume
   char      pendingUnsubId[CHANNEL_ID_LEN];   // what the confirmation is about, copied out of the tile
   char      pendingUnsubName[CHANNEL_NAME_LEN];
} home;

static Font        font;
static Label       titleLabel, statusLabel, pageLabel;
static Label       noticeLabel, noticeHintLabel;
static ButtonHints hints;

// The Subscriptions tab lists the CHANNELS you follow, one tile each, rather than merging their videos into
// a single feed - a feed like that buries a channel you just subscribed to among everything else, which is
// exactly how a new subscription looks like it did not register. X opens a tile's channel.
//
// Each channel is asked for its latest page, which supplies the channel's real title and a thumbnail. A
// channel whose fetch fails still gets a tile, built from the name stored when it was subscribed, so a
// subscription is never invisible. Bails if the source was swapped away mid-fetch (one request per channel),
// so switching category stays snappy.
static int fetchSubscriptionChannels(SearchResults *out)
{
   memset(out, 0, sizeof *out);

   Subscription *channels = (Subscription *)malloc(MAX_SUBSCRIPTIONS * sizeof *channels);
   if (!channels) return -1;
   int channelCount = getSubscriptionList(channels, MAX_SUBSCRIPTIONS);
   if (channelCount <= 0) { free(channels); return -1; }   // 0 = none, -1 = unreadable; neither is a feed

   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   int aborted = 0;
   for (int c = 0; c < channelCount && out->count < MAX_SEARCH_RESULTS; c++) {
      if (gridStopRequested(&home.grid)) { aborted = 1; break; }

      SearchResult *item = &out->items[out->count++];
      memset(item, 0, sizeof *item);
      strCopy(item->channelId, sizeof item->channelId, channels[c].id);
      strCopy(item->title, sizeof item->title, channels[c].name[0] ? channels[c].name : channels[c].id);

      if (!page || getChannelVideos(channels[c].id, NULL, page) != 0) continue;   // keep the stored name
      if (page->channelName[0]) strCopy(item->title, sizeof item->title, page->channelName);
      if (page->count > 0) {
         strCopy(item->videoId, sizeof item->videoId, page->items[0].videoId);   // tile art: latest upload
         snprintf(item->author, sizeof item->author, "Latest: %s", page->items[0].title);
      }
   }
   free(page);
   free(channels);
   out->continuation[0] = 0;   // the list is however many channels you follow - there is no next page
   // a run that was cut short is reported as a failure, not as a short list: a partial list would be shown
   // and then cached as authoritative, permanently missing whichever channels never got their turn.
   if (aborted) return -1;
   return out->count > 0 ? 0 : -1;
}

static int homeFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (home.category == HOME_SUBS)       return fetchSubscriptionChannels(out);
   if (home.category == HOME_CONTINUE)   return getContinueWatching(out) > 0 ? 0 : -1;
   if (home.category == HOME_WATCHLATER) return getWatchLater(out) > 0 ? 0 : -1;
   if (home.category == HOME_HISTORY)    return getWatchHistory(out) > 0 ? 0 : -1;
   return getTrending(home.category, token, out);
}

static void setHeading(void)
{
   char heading[128];
   if      (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, "Subscriptions");
   else if (home.category == HOME_CONTINUE)   snprintf(heading, sizeof heading, "Continue watching");
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, "Watch Later");
   else if (home.category == HOME_HISTORY)    snprintf(heading, sizeof heading, "History");
   else                                       snprintf(heading, sizeof heading, "Trending \xe2\x80\xa2 %s", TrendingNames[home.category]);
   setLabelText(&titleLabel, heading);
   setButtonHintCaption(&hints, CROSS_HINT,    home.category == HOME_SUBS ? "Open channel" : "Play");
   setButtonHintCaption(&hints, TRIANGLE_HINT, home.category == HOME_SUBS ? "Unsubscribe"  : "Channel");
}

static void loadCategory(void)
{
   setHeading();
   if (categoryCached[home.category]) setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
   else                               setGridSource(&home.grid, homeFetch, NULL);
}

// Which rail entry the feed on screen belongs to. Trending covers three categories (Gaming/Sports/Podcasts)
// under one entry, because they are one idea to a viewer and L1/R1 already walks between them.
static SidebarItem itemForCategory(int category)
{
   if (category == HOME_SUBS)       return SIDE_SUBS;
   if (category == HOME_CONTINUE)   return SIDE_CONTINUE;
   if (category == HOME_WATCHLATER) return SIDE_LATER;
   if (category == HOME_HISTORY)    return SIDE_HISTORY;
   return SIDE_TRENDING;
}

// -1 for entries that are not a feed at all (Search, Settings, Playlists).
static int categoryForItem(SidebarItem item)
{
   switch (item) {
   case SIDE_TRENDING: return home.category < TREND_COUNT ? home.category : 0;   // keep the trending tab you were on
   case SIDE_SUBS:     return HOME_SUBS;
   case SIDE_CONTINUE: return HOME_CONTINUE;
   case SIDE_LATER:    return HOME_WATCHLATER;
   case SIDE_HISTORY:  return HOME_HISTORY;
   default:            return -1;
   }
}

// cache the current category (only when it's fully loaded) so returning to it is instant.
static void cacheCurrent(void)
{
   if (gridStage(&home.grid) != GRID_READY || gridBusy(&home.grid)) return;   // don't copy mid-append
   categoryCache[home.category] = *gridResults(&home.grid);
   categoryCached[home.category] = 1;
}

// ---- modal box: a plain notice, or a yes/no confirmation ----
//
// Same shape as search.c's. While it is up it owns the pad, so nothing underneath reacts to the press that
// dismisses it. The labels are RAW: a channel name is arbitrary text from YouTube, and an unmatched "{" in
// it would otherwise be eaten by the {i}/{b}/{color=} markup parser - swallowing the rest of the sentence in
// the very box that asks you to confirm something irreversible.
static void showNotice(const char *text)
{
   setLabelText(&noticeLabel, text);
   setLabelText(&noticeHintLabel, "Press X to close");
   home.noticeIsConfirm = 0;
   home.noticeVisible   = 1;
}

// Triangle on a subscription tile. The tile's TITLE is the channel name; its author is "Latest: <newest
// upload>" (see fetchSubscriptionChannels), so a question built from author would name a video instead of
// the channel you are about to drop. Both fields are copied out by value because the grid they point into
// is rebuilt before the answer arrives.
static void askUnsubscribe(const SearchResult *item)
{
   if (!item->channelId[0]) { showNotice("This tile has no channel id, so there is nothing to unsubscribe from."); return; }
   strCopy(home.pendingUnsubId,   sizeof home.pendingUnsubId,   item->channelId);
   strCopy(home.pendingUnsubName, sizeof home.pendingUnsubName, item->title);

   char message[256];
   snprintf(message, sizeof message, "Unsubscribe from %s?", home.pendingUnsubName);
   setLabelText(&noticeLabel, message);
   setLabelText(&noticeHintLabel, "X Unsubscribe     O Cancel");
   home.noticeIsConfirm = 1;
   home.noticeVisible   = 1;
}

// X on the confirmation. Refreshing the list does NOT refetch: the subscriptions feed costs one HTTP
// request per channel (see fetchSubscriptionChannels), and the list we already have, minus one entry, is
// exactly what a refetch would return.
static void unsubscribeConfirmed(void)
{
   int revisionBefore = getSubscriptionsRevision();
   int rc = setSubscribed(home.pendingUnsubId, NULL, 0);

   if (rc != SUBSCRIBE_OK) {
      char message[256];
      if (rc == SUBSCRIBE_ERR_WRITE && !isStorageWritable())
         snprintf(message, sizeof message,
                  "The data folder could not be created or written at startup, so nothing can be saved.\n\n%s",
                  getSubscriptionsPath());
      else if (rc == SUBSCRIBE_ERR_WRITE)
         snprintf(message, sizeof message,
                  "Unsubscribed - but it could not be saved, so the channel is back after a restart.\n\n%s\n\nFree space: %lu KB",
                  getSubscriptionsPath(), getStorageFreeKB());
      else
         snprintf(message, sizeof message, "Could not unsubscribe from %s.", home.pendingUnsubName);
      showNotice(message);
      return;   // the list on screen still matches what is stored, so leave it alone
   }

   // setSubscribed also answers OK for a channel that was not in the list at all, and that case does not
   // move the revision - so the revision, not the return code, is what proves something was removed.
   if (getSubscriptionsRevision() == revisionBefore) {
      showNotice("That channel was not in the subscription list.");
      return;
   }
   subsRevisionSeen = getSubscriptionsRevision();

   SearchResults *kept = &categoryCache[HOME_SUBS];
   *kept = *gridResults(&home.grid);
   int out = 0;
   for (int i = 0; i < kept->count; i++)
      if (strcmp(kept->items[i].channelId, home.pendingUnsubId) != 0) kept->items[out++] = kept->items[i];
   kept->count = out;

   // an empty cached feed would be handed to the grid as a READY page with nothing on it: no tiles, no
   // "No results", no selection. Let the normal load path report the empty list instead.
   if (kept->count == 0) { categoryCached[HOME_SUBS] = 0; loadCategory(); return; }
   categoryCached[HOME_SUBS] = 1;
   loadCategory();
}

// X on a subscription tile opens the channel. openChannelFor reads the channel's NAME from author, which on
// these tiles is "Latest: <newest upload>" - so the channel screen was headed with a video title, and
// re-subscribing there (the obvious undo after a mis-press) would store that title as the channel's name.
// The tile's title IS the channel name, so hand over a copy carrying it.
static void openSelectedChannel(const SearchResult *item)
{
   cacheCurrent();
   if (home.category != HOME_SUBS) { openChannelFor(item); return; }
   SearchResult channel = *item;
   strCopy(channel.author, sizeof channel.author, item->title);
   openChannelFor(&channel);
}

static void switchCategory(int delta)
{
   cacheCurrent();
   home.category = (home.category + delta + HOME_CAT_COUNT) % HOME_CAT_COUNT;
   loadCategory();
}

// OSK confirmed: a link plays directly, anything else opens a search.
static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (strstr(text, "youtu")) { stopVideoGrid(&home.grid); playVideo(text); return; }
   cacheCurrent();
   openSearchQuery(text);
}

static void initHome(void)
{
   home.screenW = getGfxScreenWidth();
   home.screenH = getGfxScreenHeight();

   font = openSystemFont(FONT_POP);
   initLabel(&titleLabel,  &font, GRID_LEFT, HEADER_Y, home.screenW - GRID_LEFT - MARGIN_X, AUTO, TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statusLabel, &font, GRID_LEFT, GRID_TOP, home.screenW - GRID_LEFT - MARGIN_X, AUTO, 21, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   initLabel(&pageLabel,   &font, 0, HEADER_Y, AUTO, AUTO, TITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");

   home.noticeVisible   = 0;
   home.noticeIsConfirm = 0;
   home.noticePanelW    = home.screenW * 2 / 3;
   initLabelRaw(&noticeLabel,     &font, 0, 0, home.noticePanelW - 2 * NOTICE_PAD, AUTO, NOTICE_TEXT, activeTheme->textPrimary,   TEXT_WRAP,   "");
   initLabelRaw(&noticeHintLabel, &font, 0, 0, AUTO, AUTO, NOTICE_HINT, activeTheme->textSecondary, TEXT_NOWRAP, "");

   home.pendingReload = 0;

   initButtonHints(&hints, &font, home.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R3),       "Download");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Settings");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L1),       "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R1),       "Category");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L2),       "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R2),       "Page");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT),   "PS button");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_DPAD_LEFT), "Menu");

   initVideoGrid(&home.grid, &font, GRID_LEFT, GRID_TOP,
                 home.screenW - GRID_LEFT - MARGIN_X, home.screenH - GRID_TOP - HINTS_RESERVE);
   setGridView(&home.grid, GRID_VIEW_LIST);   // the list is the only layout now - see sidebar.h
   initSidebar(&font, home.screenH);
   setGridWatchedPredicate(&home.grid, isWatched);
   setGridWatchLaterPredicate(&home.grid, isWatchLater);
   setGridProgressSource(&home.grid, getWatchedPosition);

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   historyRevisionSeen = getWatchHistoryRevision();
   home.category = HOME_SUBS;   // always start on Subscriptions
   loadCategory();
}

static void updateHome(void)
{
   if (home.pendingReload) { home.pendingReload = 0; loadCategory(); }

   updateDownloadOverlay();
   if (oskInputActive()) return;

   if (home.noticeVisible) {   // modal: nothing underneath sees the pad until it is dismissed
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         home.noticeVisible = 0;
         // clear the flag BEFORE acting - the unsubscribe may put an error box up in its place
         if (home.noticeIsConfirm) { home.noticeIsConfirm = 0; unsubscribeConfirmed(); }
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { home.noticeVisible = 0; home.noticeIsConfirm = 0; }
      return;
   }

   // ---- the left rail ----
   //
   // It is entered with LEFT or O and owns the d-pad while it has focus. The grid is NOT skipped in the
   // meantime, only muted: updateVideoGrid also reaps the fetch worker and decodes thumbnails, so returning
   // early here would freeze a feed mid-load - exactly when someone reaches for the rail to go elsewhere.
   setSidebarCurrent(itemForCategory(home.category));
   if (isSidebarFocused()) {
      int picked = updateSidebar();
      setGridInputSuppressed(&home.grid, 1);
      updateVideoGrid(&home.grid);
      if (picked >= 0) {
         if (!isSidebarItemEnabled(picked)) {
            showNotice("Playlists are not built yet - they are the next thing coming.");
         } else if (picked == SIDE_SEARCH) {
            setSidebarFocused(0);
            openSearchInput(NULL, onQueryEntered);
         } else if (picked == SIDE_SETTINGS) {
            setSidebarFocused(0);
            openSettingsScreen();
         } else {
            int category = categoryForItem(picked);
            setSidebarFocused(0);
            if (category >= 0 && category != home.category) { cacheCurrent(); home.category = category; loadCategory(); }
         }
      }
      if (!isSidebarFocused()) setGridInputSuppressed(&home.grid, 0);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_LEFT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      setSidebarFocused(1);
      setGridInputSuppressed(&home.grid, 1);
      return;
   }

   // SELECT presses the PS button for pads that have none the console listens to (DualShock 4 / DualSense).
   if (isPadButtonPressed(PAD_BTN_SELECT)) {
      if (pressPsButton() != 0)
         showNotice("The PS button could not be sent: this console refused to register a virtual controller.");
      return;
   }

   // START is the settings now that Search has moved into the rail
   if (isPadButtonPressed(PAD_BTN_START)) { openSettingsScreen(); return; }

   // L1/R1 stay responsive even mid-load (the source swap is non-blocking)
   if      (isPadButtonPressed(PAD_BTN_R1)) { switchCategory(1);  return; }
   else if (isPadButtonPressed(PAD_BTN_L1)) { switchCategory(-1); return; }

   updateVideoGrid(&home.grid);
   if (gridStage(&home.grid) != GRID_READY) return;

   // the feed on screen now reflects these revisions - only at this point is a change really "seen"
   if (!gridBusy(&home.grid)) {
      if (home.category == HOME_SUBS)       subsRevisionSeen = getSubscriptionsRevision();
      if (home.category == HOME_WATCHLATER) watchLaterRevisionSeen = getWatchLaterRevision();
      if (home.category == HOME_HISTORY ||
          home.category == HOME_CONTINUE)   historyRevisionSeen = getWatchHistoryRevision();
   }

   const SearchResult *selected = gridSelected(&home.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      toggleWatchLater(selected);
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) loadCategory();   // reflect the add/remove immediately
      return;
   }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) enqueueDownload(selected); return; }   // live can't be downloaded
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      // In Subscriptions a tile IS a channel and X already opens it (just below), so Triangle was doing the
      // exact same thing as X here. It is also the button that unsubscribes one level down, inside the
      // channel screen - so this keeps the meaning and only drops the two presses it took to get there.
      if (home.category == HOME_SUBS) { askUnsubscribe(selected); return; }
      openSelectedChannel(selected);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      // in the Subscriptions tab a tile is a channel, so X opens it instead of playing anything
      if (home.category == HOME_SUBS) { openSelectedChannel(selected); return; }
      markWatchedItem(selected);   // full entry, so History can list it
      stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
      // hand over the whole list so autoplay can carry on with what follows
      playVideoFromList(gridResults(&home.grid), gridSelectedIndex(&home.grid));
   }
}

static void drawHome(void)
{
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->appBg);

   // Order matters. The grid goes down first, then the rail OVER it (expanded, it covers the first column
   // rather than pushing it aside), and only then the heading, the page counter and the status line - those
   // three start at the grid's left edge, which is INSIDE the expanded panel, and "Loading..." is the only
   // feedback there is while a feed is on its way. Covering it with the menu would leave a blank screen.
   if (gridStage(&home.grid) == GRID_READY) drawVideoGrid(&home.grid);
   drawSidebar(home.screenH);

   drawLabel(&titleLabel);
   if (gridStage(&home.grid) == GRID_READY) {
      char page[40];
      formatGridPage(&home.grid, page, sizeof page);
      setLabelText(&pageLabel, page);   // only rasterises when the text actually changes
      moveLabel(&pageLabel, home.screenW - MARGIN_X - pageLabel.tt.tex.w, HEADER_Y);
      drawLabel(&pageLabel);
   } else {
      setLabelText(&statusLabel, gridStage(&home.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }
   drawButtonHints(&hints, home.screenW);
   drawDownloadOverlay(home.screenW);

   if (home.noticeVisible) {
      int panelW = home.noticePanelW;
      int panelH = noticeLabel.tt.tex.h + 2 * NOTICE_PAD + NOTICE_HINT + 18;
      int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;
      int ring = activeTheme->focusThickness;
      fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->scrim);
      fillGfxRectangle(x - ring, y - ring, panelW + 2 * ring, panelH + 2 * ring, activeTheme->focusBorder);
      fillGfxRectangle(x, y, panelW, panelH, activeTheme->surface);
      drawLabelAt(&noticeLabel, x + NOTICE_PAD, y + NOTICE_PAD);
      drawLabelAt(&noticeHintLabel, x + NOTICE_PAD, y + panelH - NOTICE_PAD - NOTICE_HINT + 6);
   }

}

static void suspendHome(void) { stopVideoGrid(&home.grid); }   // free the http pools for the pushed screen

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   // a subscribe/unsubscribe or watch-later change while we were away makes that cached feed stale.
   //
   // Both are checked before anything reloads: returning early out of the subscriptions branch used to skip
   // the watch-later check entirely, so a visit that did both left the second one stale.
   //
   // The "seen" markers are NOT advanced here. They record what the DISPLAYED feed reflects, and are moved
   // on only once a feed has actually loaded (see updateHome). Consuming them at the start of a refetch
   // spent a subscribe's one and only retry on an attempt that might still fail or be cut short, and the
   // channel was then missing for the rest of the session however often you left and came back.
   int subsStale    = getSubscriptionsRevision() != subsRevisionSeen;
   int laterStale   = getWatchLaterRevision() != watchLaterRevisionSeen;
   int historyStale = getWatchHistoryRevision() != historyRevisionSeen;
   if (subsStale)    categoryCached[HOME_SUBS] = 0;
   if (laterStale)   categoryCached[HOME_WATCHLATER] = 0;
   if (historyStale) categoryCached[HOME_HISTORY] = 0;
   if (historyStale) categoryCached[HOME_CONTINUE] = 0;   // built from the same positions

   int showingStale = (subsStale && home.category == HOME_SUBS) ||
                      (laterStale && home.category == HOME_WATCHLATER) ||
                      (historyStale && (home.category == HOME_HISTORY || home.category == HOME_CONTINUE));

   // Do NOT reload here. Autoplay resumes this screen only to pass straight through it: advanceAutoplay
   // pops the finished player, which lands here, and immediately pushes the next one - and the push calls
   // suspendHome -> stopVideoGrid, which JOINS the worker a reload has just started. That worker is four
   // in-flight thumbnail requests, so the whole render loop would freeze at every video boundary, with those
   // connections racing the next video's stream. Deferring to updateHome costs nothing (it runs on the next
   // frame this screen is actually in front) and never fires on a pass-through.
   if (showingStale || gridStage(&home.grid) != GRID_READY) home.pendingReload = 1;
}

static void termHome(void)
{
   termVideoGrid(&home.grid);
   termSidebar();
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&statusLabel);
   freeLabel(&pageLabel);
   freeLabel(&noticeLabel);
   freeLabel(&noticeHintLabel);
   closeFont(&font);
}

void openHome(void) { changeScreen(&homeScreen); }

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };

// home - the landing screen: trending categories, live streams and your own lists in a VideoGrid (see home.h).

#include "screens/home.h"
#include "screens/search.h"
#include "screens/search-input.h"
#include "screens/settings-screen.h"
#include "sidebar.h"
#include "screens/playlist-screen.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "settings.h"           // the content region: a new one makes the Trending and Live lists stale
#include "account.h"            // signed in: For you, and Subscriptions / Watch Later / History from the account
#include "screens/account-screen.h"   // Login in the rail while signed out: the sign-in
#include "screens/account-page.h"     // Account in the rail while signed in: the account's page
#include "downloads.h"
#include "update-check.h"   // (V3.0.0 Test 12) a newer release is out: said once a start
#include "screens/welcome-screen.h"   // (Test 15) a new version's changes, once
#include "app-paths.h"      // APP_VERSION, in that notice
#include "screens/download-choice.h"   // R3: in which form to download
#include "ps-button.h"
#include "ui/console-glyphs.h"
#include "ui-fonts.h"           // the shared fonts, label helpers, text metrics
#include "ui-draw.h"            // safe area, rounded panel, chips, focus ring, equal-width digits
#include "ui-hints.h"           // the Ember hint rows
#include "ui-clock.h"           // the header clock

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"                // logWarn
#include <sys/sys_time.h>       // sys_time_get_system_time: how old a Live page is
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// The page is Bild 4's: the cards from x 176, next to the collapsed rail, under a one-line header. On the
// left of that line the page title (on Trending its three categories follow as chips, on Live its six); on
// the right the X / Square / Triangle hints, the page counter and the clock. Everything on it has its
// capitals centred on the chips' band, HEADER_Y .. HEADER_Y + HEADER_H, which puts the title's baseline at
// 107 as in the design. The other hints sit at the bottom right; "Loading..." and "No results" sit where the
// cards start.
#define CONTENT_X     SIDEBAR_CONTENT_X   // 176
#define HEADER_Y      67
#define HEADER_H      45
#define HEADER_HINTS_Y (HEADER_Y + (HEADER_H - UI_HINT_GLYPH_H) / 2)   // the top hint row, centred on the band
#define HEADER_GAP    40        // between the top hint row, the page counter and the clock (Bild 1's clock
                                // sits 40 after the hints before it)
#define TITLE_GAP     48        // the title or its last chip -> the top hint row, at the least
#define TITLE_MIN_W   320       // the top hint row narrows its gaps before the title's room drops below this
#define CHIP_TITLE_GAP 28       // Trending's title -> its first chip (Bild 4)
#define CHIP_PAD_X    20
#define CHIP_PAD_MIN  12        // Live's six chips may close up to this before any is left out (fitLiveChips)
#define CHIP_GAP      10
#define CHIP_SIZE     20        // 20/700
#define CHIP_WEIGHT   UI_WEIGHT_BOLD
#define CLOCK_SIZE    28        // ui-clock.h's clock is 28/700: needed to put its capitals on the band
#define PAGE_SIZE     24        // page counter: 24/700 in textSecondary, its numbers in equal-width digits
#define PAGE_WEIGHT   UI_WEIGHT_BOLD
#define STATUS_SIZE   24        // "Loading..." / "No results": 24/700 in textSecondary, where the cards start
#define STATUS_WEIGHT UI_WEIGHT_BOLD
#define GRID_TOP      160
// (V3.0.0) the Home preview (setGridHero): the selected video big above one row of cards, as TEE's mockup has it
#define CARDS_TOP     630
#define STATUS_BASELINE (GRID_TOP + 30)
// The grid's rectangle ends above the bottom hint row. The grid lays out as many whole rows of cards as fit
// it - two, the second ending at y 864 at the most - so the rectangle, not luck, keeps the cards off the hints.
#define GRID_BOTTOM   (UI_HINTS_BOTTOM_Y - 16)
#define HINT_MIN_GAP  14        // a hint row narrows its gaps down to this rather than overlap anything
#define CROSS_HINT    0         // X's slot in the TOP hint row (see initHome): retitled per category
#define TRIANGLE_HINT 2         // Triangle's slot in the top row: "Channel" everywhere, "Unsubscribe" in Subscriptions
#define R3_HINT       0         // R3's slot in the BOTTOM row: hidden on Live, where nothing can be downloaded
// A Live page lists what is on air NOW. Coming back to one older than this, it is fetched again (resumeHome).
#define LIVE_MAX_AGE_US (10ull * 60ull * 1000000ull)
// The chip labels are rendered in white and tinted when drawn, so the chip on screen and the others share one
// texture each (re-colouring a rendered label means rasterising it again).
#define TEXT_TINTABLE 0xFFFFFFFFu
// The modal box, laid out like search.c's and playlist-screen.c's: the message, then its answers as a row of
// console glyphs with captions.
#define NOTICE_PAD_X      44    // inner padding left and right
#define NOTICE_PAD_TOP    40    // panel top -> the message's first capitals
#define NOTICE_PAD_BOTTOM 30    // the answers' glyph row -> panel bottom
#define NOTICE_ANSWERS_DY 34    // the message's last baseline -> the top of the answers' glyph row: a 24-px
                                // line's descender room and the 24 the other boxes leave under it
#define NOTICE_TEXT       24    // body text, 22-24/400
#define NOTICE_RADIUS     26    // the radius the row ring (UI_RING_ROW) round the box is made for

static const char *TrendingNames[TREND_COUNT] = { "Gaming", "Sports", "Podcasts" };

// home categories = the trending feeds plus a Subscriptions feed and the Watch Later queue.
//
// Signed in (account.h), Subscriptions, Watch Later and History come from the YouTube account instead of the
// console - Subscriptions then as the newest uploads of the channels followed, as YouTube shows it, not one tile per
// channel - and For you holds the account's recommendations (in the rail under Search, and on the account page).
// Continue watching and the console's playlists stay the console's. Signed out, For you is left out of the L1/R1
// ring, and picking it in the rail leads to the sign-in.
#define HOME_SUBS        TREND_COUNT
#define HOME_CONTINUE    (TREND_COUNT + 1)
#define HOME_WATCHLATER  (TREND_COUNT + 2)
#define HOME_HISTORY     (TREND_COUNT + 3)
#define HOME_PLAYLIST    (TREND_COUNT + 4)
#define HOME_FORYOU      (TREND_COUNT + 5)
#define HOME_CAT_COUNT   (TREND_COUNT + 6)   // the cached categories - and the ring L1/R1 walk outside Live

// The Live page's six chips are numbered after every cached category, so nothing above moves and the cache
// arrays keep their eight slots (six more would be about 1 MB that is never used): a live list is out of date
// within minutes, so it is never cached (loadCategory, cacheCurrent). L1/R1 keep to these six (switchCategory).
// Keep LiveNames in the order of LiveCategory (extractor.h).
#define HOME_LIVE_FIRST  HOME_CAT_COUNT
static const char *LiveNames[LIVE_COUNT] = { "Popular", "Gaming", "Sports", "Podcasts", "News", "Music" };

static int isLiveCategory(int category)
{
   return category >= HOME_LIVE_FIRST && category < HOME_LIVE_FIRST + LIVE_COUNT;
}

// (V3.0.0) the app's own downloads (downloads.h): read from disk every time, so never cached, and outside the
// L1/R1 ring - the rail leads there
#define HOME_DOWNLOADS   (HOME_LIVE_FIRST + LIVE_COUNT)
static unsigned downloadsRevisionSeen;
static char     pendingDeleteId[16];   // the download the notice asks about ...
static char     pendingMoveId[16];     // ... or the one it asks to move into the XMB
static char     pendingCancelId[16];   // ... or the one on its way it asks to stop (V3.0.0)
#define SQUARE_HINT 1                  // Square's slot in the top hint row: "To XMB" on the Downloads page

// each category's fully-loaded results, so returning to a tab skips the feed fetch (thumbnails still refetch).
static SearchResults categoryCache[HOME_CAT_COUNT];
static int           categoryCached[HOME_CAT_COUNT];
static int           subsRevisionSeen;         // subscriptions revision the cached HOME_SUBS feed was built at
static int           watchLaterRevisionSeen;   // watch-later revision the cached HOME_WATCHLATER feed was built at
static int           historyRevisionSeen;      // watch-history revision the cached HOME_HISTORY feed was built at
static int           playlistRevisionSeen;     // playlists revision the cached HOME_PLAYLIST feed was built at
static unsigned      regionRevisionSeen;       // content-region revision the cached Trending feeds were fetched at
static unsigned      accountRevisionSeen;      // sign-in revision the cached account pages were fetched at
static unsigned      accountChangesSeen;       // the account changes (Square, Triangle) the cached pages reflect
static int           openPlaylist;             // which list HOME_PLAYLIST is showing
static int           playlistIsAccount;        // ... or a YouTube playlist of the account, this one:
static char          accountPlaylistId[ACCOUNT_PLAYLIST_ID_CAP], accountPlaylistTitle[96];

static struct {
   VideoGrid grid;
   int       category;
   int       screenW, screenH;
   int       noticeVisible;                    // a modal box is up and owns the pad
   int       noticeIsConfirm;                  // X carries out the pending unsubscribe (else it just closes)
   int       noticeInterrupted;                // (V3.0.0) the box asks about downloads that did not finish
   int       interruptedChecked;               // ... asked (or found none) once this run
   int       noticePanelW;
   int       pendingReload;                    // a stale feed to reload on the next update, not on resume
   int       forYouAfterSignIn;                // For you was picked signed out: go there once the sign-in is done
   char      pendingUnsubId[CHANNEL_ID_LEN];   // what the confirmation is about, copied out of the tile
   char      pendingUnsubName[CHANNEL_NAME_LEN];
   uint64_t  liveLoadedUs;                     // when the Live page on screen asked for its first page
   int       liveChipPad;                      // the Live chips' padding, and the run of them the header
   int       liveChipFirst, liveChipLast;      //   has room for (fitLiveChips)
} home;

static Label    titleLabel, statusLabel;
static Label    pageWordLabel, pagePlusLabel;   // "Page" and the "+" of a feed that can still grow ...
static UiDigits pageDigits;                     // ... round the numbers, which step in equal cells
static Label    chipLabels[TREND_COUNT];        // Trending's categories, rendered white and tinted per state
static Label    liveChipLabels[LIVE_COUNT];     // Live's, likewise
static Label    noticeLabel;
static UiHints  closeHints, confirmHints;       // the notice's answers: X Close, or X Unsubscribe / O Cancel
static UiHints  interruptedHints;               // (V3.0.0) X Restart, Triangle Delete, O Later
// Two hint rows, as on the Cinema start (Bild 1): X / Square / Triangle - what acts on the selected tile - at
// the top right, everything else at the bottom right. At the Ember sizes the twelve no longer fit one line
// inside the safe area.
static UiHints  hintsTop, hintsBottom;
static int      pageMaxW;   // the widest the page counter can get; the top hint row keeps clear of it

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
      if (page->count > 0)   // (V3.0.0) the tile is the channel's picture (its page gave it); the latest upload in words
         snprintf(item->author, sizeof item->author, "Latest: %s", page->items[0].title);
   }
   free(page);
   free(channels);
   out->continuation[0] = 0;   // the list is however many channels you follow - there is no next page
   // a run that was cut short is reported as a failure, not as a short list: a partial list would be shown
   // and then cached as authoritative, permanently missing whichever channels never got their turn.
   if (aborted) return -1;
   return out->count > 0 ? 0 : -1;
}

// A Subscriptions tile is a CHANNEL (see above): its picture is the newest upload's, but whether you watched
// that upload says nothing about the channel. Asked about the upload's id, the grid dimmed the whole tile and
// badged it WATCHED, so on that tab the answer is always no.
static int tileWatched(const char *videoId)
{
   return home.category != HOME_SUBS && isWatched(videoId);
}

// Signed in, Subscriptions is a channel per tile as well, built from the account's subscription feed - the newest
// uploads of every channel it follows, newest first. A channel's first video there is its latest upload, which
// gives the tile its picture, and the feed's order puts the channels with something new first, as YouTube's own
// list of them does; a feed that just mixed every upload together read as a jumble (TEE, on the PS3). There is no
// request per channel - an account may follow hundreds. Feed pages are read until there are two screens of
// channels; the next page carries on down the feed, and a channel already shown is never shown again.
#define SUBS_FEED_PAGES 4
static char subsShown[MAX_SEARCH_RESULTS][CHANNEL_ID_LEN];   // the grid worker's: one fetch runs at a time
static int  subsShownCount;
static int  subsFromFeed;   // (V3.0.0) page 1 came from the feed (the channel list failed): its next pages do too

static int fetchSubscriptionsFromFeed(const char *token, SearchResults *out);   // below: the way before V3.0.0

// (V3.0.0) Signed in, Subscriptions is the account's own list of the channels it follows (FEchannels): every one of
// them - 339 on TEE's account - in one request, each tile the channel's name and picture, "@handle . subscribers"
// under it (TEE: "wirklich nur die channels ... mit den jeweiligen namen", then "All, profile pictures"). Built from
// the feed below, the page had asked for 30 pages of uploads and still showed only the channels that had uploaded
// lately. Should the list not come, the feed's channels stand in.
static int fetchAccountSubscriptionChannels(const char *token, SearchResults *out)
{
   if (!token) subsFromFeed = 0;
   if (!subsFromFeed) {
      if (getAccountFeed(ACCOUNT_FEED_CHANNELS, token, out) == 0 && (out->count > 0 || token)) return 0;
      if (token) return -1;
      logWarn("[home] the account's channel list did not come: the channels of its subscription feed instead\n");
      subsFromFeed = 1;
   }
   return fetchSubscriptionsFromFeed(token, out);
}

static int fetchSubscriptionsFromFeed(const char *token, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   if (!token) subsShownCount = 0;   // page 1: nothing is on screen yet
   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   char *next = (char *)malloc(MAX_CONTINUATION);
   if (!page || !next) { free(page); free(next); return -1; }
   strCopy(next, MAX_CONTINUATION, token ? token : "");
   int rc = 0, wanted = 2 * home.grid.cols * home.grid.visibleRows;
   for (int read = 0; read < SUBS_FEED_PAGES && out->count < wanted; read++) {
      if (gridStopRequested(&home.grid)) { rc = -1; break; }   // cut short counts as failed, as above
      if (getAccountFeed(ACCOUNT_FEED_SUBSCRIPTIONS, next[0] ? next : NULL, page) != 0) {
         if (read == 0) rc = -1;   // a later page that fails just ends the list here
         next[0] = 0;
         break;
      }
      for (int i = 0; i < page->count && out->count < MAX_SEARCH_RESULTS; i++) {
         const SearchResult *video = &page->items[i];
         if (!video->channelId[0]) continue;
         int seen = 0;
         for (int s = 0; s < subsShownCount && !seen; s++) seen = strcmp(subsShown[s], video->channelId) == 0;
         if (seen) continue;
         if (subsShownCount < MAX_SEARCH_RESULTS) strCopy(subsShown[subsShownCount++], CHANNEL_ID_LEN, video->channelId);
         SearchResult *tile = &out->items[out->count++];
         memset(tile, 0, sizeof *tile);
         strCopy(tile->channelId, sizeof tile->channelId, video->channelId);
         strCopy(tile->title, sizeof tile->title, video->author[0] ? video->author : video->channelId);
         strCopy(tile->videoId, sizeof tile->videoId, video->videoId);   // tile art: the latest upload (no picture here)
         snprintf(tile->author, sizeof tile->author, "Latest: %s", video->title);
      }
      strCopy(next, MAX_CONTINUATION, page->continuation);
      if (!next[0]) break;
   }
   strCopy(out->continuation, sizeof out->continuation, next);
   free(page);
   free(next);
   return rc;
}

// The Watch Later tag on a card: the account's list while signed in, the console's otherwise.
static int tileInWatchLater(const char *videoId)
{
   return isAccountSignedIn() ? isInAccountWatchLater(videoId) : isWatchLater(videoId);
}

// Subscriptions shows one tile per CHANNEL, signed in or not: X opens it, Triangle unsubscribes.
static int subsAreChannels(void) { return home.category == HOME_SUBS; }

// A source swap (L1/R1, the rail, START) asked the fetch to stop: a Live chip that falls back to a second
// request asks this before making it, so a switch does not wait for both (getLiveFeed).
static int homeFetchStopped(void) { return gridStopRequested(&home.grid); }

static int homeFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   // This runs on the grid's worker while the UI thread may switch the page, so the category is read ONCE: two
   // reads could fall either side of a switch and ask a Live feed for a category that is not one.
   int category = home.category, signedIn = isAccountSignedIn();
   if (category == HOME_FORYOU)     return getAccountFeed(ACCOUNT_FEED_HOME, token, out);
   if (category == HOME_SUBS)
      return signedIn ? fetchAccountSubscriptionChannels(token, out) : fetchSubscriptionChannels(out);
   if (category == HOME_CONTINUE)   return getContinueWatching(out) > 0 ? 0 : -1;
   if (category == HOME_DOWNLOADS)  return listDownloads(out) > 0 ? 0 : -1;
   if (category == HOME_WATCHLATER)
      return signedIn ? getAccountFeed(ACCOUNT_FEED_WATCH_LATER, token, out) : getWatchLater(out) > 0 ? 0 : -1;
   if (category == HOME_HISTORY)
      return signedIn ? getAccountFeed(ACCOUNT_FEED_HISTORY, token, out) : getWatchHistory(out) > 0 ? 0 : -1;
   if (category == HOME_PLAYLIST) {
      if (playlistIsAccount) return getAccountPlaylistVideos(accountPlaylistId, token, out);
      return getPlaylistItems(openPlaylist, out) > 0 ? 0 : -1;
   }
   if (isLiveCategory(category)) {
      int rc = getLiveFeed((LiveCategory)(category - HOME_LIVE_FIRST), token, out, homeFetchStopped);
      // As with Subscriptions: a first page cut short by a stop - a News chip whose second request was skipped -
      // counts as a failure, not as the whole page, so it is fetched again once Home is back on top (resumeHome
      // reloads a page that is not READY). A source swap throws the page away in any case.
      if (rc == 0 && !token && homeFetchStopped()) return -1;
      return rc;
   }
   return getTrending((TrendingCategory)category, token, out);
}

// ---- the header's right end ----
//
// From the right: the clock on the safe edge, the page counter HEADER_GAP to the left of the clock's widest
// form, and the top hint row HEADER_GAP to the left of the counter's widest. Both widths are fixed for the
// session (equal-width digits, the largest page count a feed can reach), so nothing on the line moves when the
// hour or the page number gains a digit.
static int pageRight(void)      { return UI_SAFE_RIGHT - uiGetClockWidth() - HEADER_GAP; }
static int hintsTopRight(void)  { return pageRight() - pageMaxW - HEADER_GAP; }
static int hintsTopLeft(void)   { return hintsTopRight() - uiGetHintsWidth(&hintsTop); }

// The page counter at its widest: as many digits a side as the most pages a feed can make (the 500-result cap
// over the grid's page size). Every digit takes the same cell, so the sample's digits do not matter.
static int measurePageMaxW(void)
{
   int size = home.grid.visibleRows * home.grid.cols;
   int pages = size > 0 ? (MAX_SEARCH_RESULTS + size - 1) / size : MAX_SEARCH_RESULTS;
   char digits[12], sample[32];
   snprintf(digits, sizeof digits, "%d", pages);
   snprintf(sample, sizeof sample, " %s / %s", digits, digits);
   return pageWordLabel.tt.tex.w + uiGetDigitsWidth(&pageDigits, sample) + pagePlusLabel.tt.tex.w;
}

// Narrows a hint row's gaps, two px at a time down to HINT_MIN_GAP, until it is no wider than maxWidth. How
// wide a row comes out is up to the console's glyph art - 36x36 for every button on a PS3, 26 to 75 x 23 in
// RPCS3, all scaled to the row's 34 px - so this is settled at run time, not here.
static void fitHintRow(UiHints *row, int maxWidth)
{
   int gap = UI_HINT_GAP;
   uiSetHintsGap(row, gap);
   while (uiGetHintsWidth(row) > maxWidth && gap > HINT_MIN_GAP) {
      gap -= 2;
      uiSetHintsGap(row, gap);
   }
}

// The title may run up to TITLE_GAP short of the top hint row. That row is wider on Subscriptions ("Open
// channel", "Unsubscribe") and its glyphs' widths are only known at run time, so the budget is worked out
// again whenever the captions change - the row giving up some of its gaps first if the title would get less
// than TITLE_MIN_W. A new width needs the old texture dropped: setLabelText skips a text the label already
// shows.
static void fitTitleWidth(void)
{
   fitHintRow(&hintsTop, hintsTopRight() - TITLE_GAP - TITLE_MIN_W - CONTENT_X);
   int width = hintsTopLeft() - TITLE_GAP - CONTENT_X;
   if (width < 1) width = 1;
   if (width != titleLabel.width) { freeLabel(&titleLabel); titleLabel.width = width; }
}

// Live chips first..last side by side, padX either side of each word and CHIP_GAP between them.
static int liveChipsWidth(int first, int last, int padX)
{
   int width = 0;
   for (int c = first; c <= last; c++) width += liveChipLabels[c].tt.tex.w + 2 * padX + (c > first ? CHIP_GAP : 0);
   return width;
}

// Live has six chips where Trending has three, about 710 px after the title. They meet the top hint row with
// a 12-hour clock, and with RPCS3's wider glyph art even without one. Worked out again whenever the heading is
// (the row's width and the chip on screen are what change), in this order until the chips fit:
//   1. the top hint row closes its gaps up, down to HINT_MIN_GAP;
//   2. the chips close up, their padding from CHIP_PAD_X down to CHIP_PAD_MIN;
//   3. the header shows the longest run of chips that holds the one on screen.
// The chip on screen is always drawn whole - should even it alone not fit, it is drawn regardless.
static void fitLiveChips(void)
{
   int active = home.category - HOME_LIVE_FIRST, last = LIVE_COUNT - 1;
   int x = CONTENT_X + titleLabel.tt.tex.w + CHIP_TITLE_GAP;
   int padX = CHIP_PAD_X, step = 0;
   while (x + liveChipsWidth(0, last, padX) > hintsTopLeft() - TITLE_GAP && hintsTop.gap > HINT_MIN_GAP) {
      int gap = hintsTop.gap - 2;
      uiSetHintsGap(&hintsTop, gap < HINT_MIN_GAP ? HINT_MIN_GAP : gap);
      step = 1;
   }
   int limit = hintsTopLeft() - TITLE_GAP;
   while (x + liveChipsWidth(0, last, padX) > limit && padX > CHIP_PAD_MIN) { padX--; step = 2; }

   int first = 0;
   if (x + liveChipsWidth(0, last, padX) > limit) {
      step = 3;
      first = last = active;
      int longest = 0;
      for (int from = 0; from <= active; from++) {
         int to = LIVE_COUNT - 1;
         while (to > active && x + liveChipsWidth(from, to, padX) > limit) to--;
         if (x + liveChipsWidth(from, to, padX) <= limit && to - from + 1 > longest) {
            longest = to - from + 1;
            first = from;
            last = to;
         }
      }
   }
   home.liveChipPad   = padX;
   home.liveChipFirst = first;
   home.liveChipLast  = last;

   // which step it took, once per outcome: with this console's glyphs and clock it is the same on every chip
   // unless it came to step 3
   static int logged[5] = { -1, -1, -1, -1, -1 };
   int outcome[5] = { step, hintsTop.gap, padX, first, last };
   if (memcmp(logged, outcome, sizeof outcome) == 0) return;
   memcpy(logged, outcome, sizeof outcome);
   if (step < 3)
      logInfo("[home] Live chips: all %d fit (step %d: hint gap %d, chip padding %d, %d px to spare)\n", LIVE_COUNT,
              step, hintsTop.gap, padX, limit - x - liveChipsWidth(0, LIVE_COUNT - 1, padX));
   else
      logWarn("[home] Live chips: only %d-%d of %d fit (step 3: hint gap %d, chip padding %d)\n", first + 1,
              last + 1, LIVE_COUNT, hintsTop.gap, padX);
}

static void setHeading(void)
{
   char heading[128];
   if      (home.category == HOME_FORYOU)     snprintf(heading, sizeof heading, "For you");
   else if (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, "Subscriptions");
   else if (home.category == HOME_CONTINUE)   snprintf(heading, sizeof heading, "Continue watching");
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, "Watch Later");
   else if (home.category == HOME_HISTORY)    snprintf(heading, sizeof heading, "History");
   else if (home.category == HOME_DOWNLOADS)  snprintf(heading, sizeof heading, "Downloads");
   else if (home.category == HOME_PLAYLIST)
      snprintf(heading, sizeof heading, "%s", playlistIsAccount ? accountPlaylistTitle : getPlaylistName(openPlaylist));
   else if (isLiveCategory(home.category))    snprintf(heading, sizeof heading, "Live");       // the category: a chip
   else                                       snprintf(heading, sizeof heading, "Trending");   // the category: a chip
   uiSetHintCaption(&hintsTop, CROSS_HINT,    subsAreChannels() ? "Open channel" : "Play");
   uiSetHintCaption(&hintsTop, TRIANGLE_HINT, subsAreChannels() ? "Unsubscribe"  :
                                              home.category == HOME_DOWNLOADS ? "Delete" : "Channel");
   // YouTube's For you tiles name no channel (no id anywhere in them), so Triangle has nothing to open there
   uiSetHintShown(&hintsTop, TRIANGLE_HINT, home.category != HOME_FORYOU);
   // A live stream cannot be downloaded, and a Live page holds nothing else: R3 does nothing there (updateHome),
   // so it has no hint either. The bottom row only gets narrower for it.
   uiSetHintShown(&hintsBottom, R3_HINT, !isLiveCategory(home.category) && home.category != HOME_DOWNLOADS &&
                  !subsAreChannels());
   // (V3.0.0) a Subscriptions tile is a channel's picture, no video: nothing to download, file or watch later
   uiSetHintShown(&hintsBottom, 1, !subsAreChannels());   // L3, Add to playlist
   uiSetHintShown(&hintsTop, SQUARE_HINT, !subsAreChannels());
   uiSetHintCaption(&hintsTop, SQUARE_HINT, home.category == HOME_DOWNLOADS ? "To XMB" : "Watch Later");
   fitTitleWidth();   // after the captions: the row's new width decides how far the title may run
   // on the playlist tab the heading is the playlist's name - the user's text, so raw and checked for Manrope
   uiSetTextLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, heading);
   if (isLiveCategory(home.category)) fitLiveChips();   // after the title: the chips start where it ends
}

static void loadCategory(void)
{
   setHeading();
   if (isLiveCategory(home.category)) {   // never cached: what is on air changes by the minute
      home.liveLoadedUs = sys_time_get_system_time();
      setGridSource(&home.grid, homeFetch, NULL);
      return;
   }
   if (home.category == HOME_DOWNLOADS) {   // never cached: it is on disk, and changes as downloads land
      downloadsRevisionSeen = getDownloadsRevision();
      setGridSource(&home.grid, homeFetch, NULL);
      return;
   }
   if (categoryCached[home.category]) setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
   else                               setGridSource(&home.grid, homeFetch, NULL);
}

// Which rail entry the feed on screen belongs to. Trending covers three categories (Gaming/Sports/Podcasts)
// under one entry, because they are one idea to a viewer and L1/R1 already walks between them; Live its six
// chips likewise.
static SidebarItem itemForCategory(int category)
{
   if (isLiveCategory(category))    return SIDE_LIVE;
   if (category == HOME_FORYOU)     return SIDE_FORYOU;
   if (category == HOME_SUBS)       return SIDE_SUBS;
   if (category == HOME_CONTINUE)   return SIDE_CONTINUE;
   if (category == HOME_WATCHLATER) return SIDE_LATER;
   if (category == HOME_HISTORY)    return SIDE_HISTORY;
   if (category == HOME_PLAYLIST)   return SIDE_PLAYLISTS;
   if (category == HOME_DOWNLOADS)  return SIDE_DOWNLOADS;
   return SIDE_TRENDING;
}

// -1 for entries that are not a feed at all (Search, Settings, Playlists).
static int categoryForItem(SidebarItem item)
{
   switch (item) {
   case SIDE_TRENDING: return home.category < TREND_COUNT ? home.category : 0;   // keep the trending tab you were on
   case SIDE_LIVE:     return isLiveCategory(home.category) ? home.category : HOME_LIVE_FIRST;   // or Popular
   case SIDE_FORYOU:   return HOME_FORYOU;
   case SIDE_SUBS:     return HOME_SUBS;
   case SIDE_CONTINUE: return HOME_CONTINUE;
   case SIDE_LATER:    return HOME_WATCHLATER;
   case SIDE_HISTORY:  return HOME_HISTORY;
   case SIDE_DOWNLOADS: return HOME_DOWNLOADS;
   default:            return -1;
   }
}

// cache the current category (only when it's fully loaded) so returning to it is instant. Live is never
// cached: it has no slot, and what it lists is stale within minutes. Nor is a feed waiting for its reload
// (pendingReload): resumeHome has just dropped its cache as stale - after a new region, say - and a pick made in
// the rail of another screen leaves the page before updateHome reloads it. Cached then, the old region's
// Trending list came back on the next visit.
static void cacheCurrent(void)
{
   if (isLiveCategory(home.category) || home.category == HOME_DOWNLOADS || home.pendingReload) return;
   if (gridStage(&home.grid) != GRID_READY || gridBusy(&home.grid)) return;   // don't copy mid-append
   categoryCache[home.category] = *gridResults(&home.grid);
   categoryCached[home.category] = 1;
}

// ---- modal box: a plain notice, or a yes/no confirmation ----
//
// Same shape as search.c's. While it is up it owns the pad, so nothing underneath reacts to the press that
// dismisses it. The message is RAW: a channel name is arbitrary text from YouTube, and an unmatched "{" in
// it would otherwise be eaten by the {i}/{b}/{color=} markup parser - swallowing the rest of the sentence in
// the very box that asks you to confirm something irreversible. It also picks its font per message, so a
// Japanese channel name falls back to FONT_POP instead of rendering blank in Manrope.
static void showNotice(const char *text)
{
   uiSetTextLabel(&noticeLabel, UI_BODY_WEIGHT, text);
   home.noticeIsConfirm = 0;   // answered by closeHints
   home.noticeVisible   = 1;
}

// The name the box calls the pending channel by. One the console has no letters for (all Thai or Arabic, say:
// uiTextIsLegible) would leave a blank in the sentence, and its tile already shows something else in its place
// (video-grid.c) - "this channel", the tile you just pressed, says it plainly.
static const char *pendingChannelName(char *out, int cap)
{
   uiCleanText(out, cap, home.pendingUnsubName);
   return out[0] && uiTextIsLegible(out) ? out : "this channel";
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

   char name[CHANNEL_NAME_LEN], message[256];
   snprintf(message, sizeof message, "Unsubscribe from %s?", pendingChannelName(name, (int)sizeof name));
   uiSetTextLabel(&noticeLabel, UI_BODY_WEIGHT, message);
   uiSetHintCaption(&confirmHints, 0, "Unsubscribe");   // (the same box asks about a download: "Delete")
   home.noticeIsConfirm = 1;   // answered by confirmHints
   home.noticeVisible   = 1;
}

// X on the confirmation. Refreshing the list does NOT refetch: the subscriptions feed costs one HTTP
// request per channel (see fetchSubscriptionChannels), and the list we already have, minus one entry, is
// exactly what a refetch would return.
static void unsubscribeConfirmed(void)
{
   // Signed in, the account unsubscribes: its tile goes at once and YouTube is told on a worker (a refusal puts the
   // channel back and says so). The page keeps what is on screen, minus the channel, rather than asking the feed
   // again this very moment, when it might still list it.
   if (isAccountSignedIn()) {
      setAccountFollowing(home.pendingUnsubId, 0);
      accountChangesSeen = getAccountChangesRevision();   // this page already shows it
      SearchResults *kept = &categoryCache[HOME_SUBS];
      *kept = *gridResults(&home.grid);
      int out = 0;
      for (int i = 0; i < kept->count; i++)
         if (strcmp(kept->items[i].channelId, home.pendingUnsubId) != 0) kept->items[out++] = kept->items[i];
      kept->count = out;
      categoryCached[HOME_SUBS] = out > 0;   // an empty page is fetched again rather than shown empty
      int at = gridSelectedIndex(&home.grid);
      loadCategory();
      keepGridSelection(&home.grid, at);   // (Test 15) on to the next channel, not back to the first
      return;
   }

   int revisionBefore = getSubscriptionsRevision();
   int rc = setSubscribed(home.pendingUnsubId, NULL, 0);

   if (rc != SUBSCRIBE_OK) {
      char name[CHANNEL_NAME_LEN], message[256];
      if (rc == SUBSCRIBE_ERR_WRITE && !isStorageWritable())
         snprintf(message, sizeof message,
                  "The data folder could not be created or written at startup, so nothing can be saved.\n\n%s",
                  getSubscriptionsPath());
      else if (rc == SUBSCRIBE_ERR_WRITE)
         snprintf(message, sizeof message,
                  "Unsubscribed - but it could not be saved, so the channel is back after a restart.\n\n%s\n\nFree space: %lu KB",
                  getSubscriptionsPath(), getStorageFreeKB());
      else
         snprintf(message, sizeof message, "Could not unsubscribe from %s.", pendingChannelName(name, (int)sizeof name));
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
   int at = gridSelectedIndex(&home.grid);
   loadCategory();
   keepGridSelection(&home.grid, at);   // (Test 15)
}

// A video Square just took out of the account's Watch Later leaves the page at once: YouTube is told on a worker,
// and a refetch this moment could still list it. The page is rebuilt from what is on screen, minus the video.
static void dropFromPage(const char *videoId)
{
   SearchResults *kept = &categoryCache[home.category];
   *kept = *gridResults(&home.grid);
   int out = 0;
   for (int i = 0; i < kept->count; i++)
      if (strcmp(kept->items[i].videoId, videoId) != 0) kept->items[out++] = kept->items[i];
   kept->count = out;
   categoryCached[home.category] = out > 0;   // an empty page is fetched again rather than shown empty (see above)
   int at = gridSelectedIndex(&home.grid);
   loadCategory();
   keepGridSelection(&home.grid, at);   // (Test 15) on to the next video, not back to the first
}

// X on a subscription tile opens the channel. openChannelFor reads the channel's NAME from author, which on
// these tiles is "Latest: <newest upload>" - so the channel screen was headed with a video title, and
// re-subscribing there (the obvious undo after a mis-press) would store that title as the channel's name.
// The tile's title IS the channel name, so hand over a copy carrying it.
static void openSelectedChannel(const SearchResult *item)
{
   cacheCurrent();
   if (!subsAreChannels()) { openChannelFor(item); return; }
   SearchResult channel = *item;
   strCopy(channel.author, sizeof channel.author, item->title);
   openChannelFor(&channel);
}

// L1/R1. Outside Live they walk the ring of the eight cached categories, Live not among them; on Live they walk
// its six chips and never leave it.
static void switchCategory(int delta)
{
   cacheCurrent();
   if (isLiveCategory(home.category))
      home.category = HOME_LIVE_FIRST + (home.category - HOME_LIVE_FIRST + delta + LIVE_COUNT) % LIVE_COUNT;
   else {
      if (home.category >= HOME_CAT_COUNT) home.category = delta > 0 ? HOME_CAT_COUNT - 1 : 0;   // Downloads
      do home.category = (home.category + delta + HOME_CAT_COUNT) % HOME_CAT_COUNT;
      while (home.category == HOME_FORYOU && !isAccountSignedIn());   // nothing to show there signed out
   }
   loadCategory();
}

// A playlist was chosen in the picker: show it as the feed. The cache is dropped first because the same
// category slot serves EVERY playlist - what is cached under it belongs to whichever list was open before.
static void onPlaylistOpened(int playlist)
{
   if (playlist < 0) return;
   cacheCurrent();
   openPlaylist = playlist;
   playlistIsAccount = 0;
   categoryCached[HOME_PLAYLIST] = 0;
   playlistRevisionSeen = getPlaylistsRevision();
   home.category = HOME_PLAYLIST;
   loadCategory();
}

// A YouTube playlist of the account was chosen: the same slot as a console list, fetched from the account.
static void onAccountPlaylistOpened(const char *id, const char *title)
{
   cacheCurrent();
   strCopy(accountPlaylistId, sizeof accountPlaylistId, id);
   strCopy(accountPlaylistTitle, sizeof accountPlaylistTitle, title);
   playlistIsAccount = 1;
   categoryCached[HOME_PLAYLIST] = 0;
   home.category = HOME_PLAYLIST;
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

   // the hints first: the title's width budget is worked out from the top row's (see fitTitleWidth)
   uiInitHints(&hintsTop);
   uiAddHint(&hintsTop, GLYPH_CROSS,    "Play");          // CROSS_HINT
   uiAddHint(&hintsTop, GLYPH_SQUARE,   "Watch Later");
   uiAddHint(&hintsTop, GLYPH_TRIANGLE, "Channel");       // TRIANGLE_HINT
   uiInitHints(&hintsBottom);
   uiAddHint(&hintsBottom, GLYPH_R3,        "Download");      // R3_HINT
   uiAddHint(&hintsBottom, GLYPH_L3,        "Add to playlist");
   uiAddHint(&hintsBottom, GLYPH_START,     "Settings");
   uiAddHint(&hintsBottom, GLYPH_L1,        "");
   uiAddHint(&hintsBottom, GLYPH_R1,        "Category");
   uiAddHint(&hintsBottom, GLYPH_L2,        "");
   uiAddHint(&hintsBottom, GLYPH_R2,        "Page");
   uiAddHint(&hintsBottom, GLYPH_SELECT,    "PS button");
   uiAddHint(&hintsBottom, GLYPH_DPAD_LEFT, "Menu");
   // The bottom row has the whole width right of the rail to itself. Its captions never change, and the
   // glyphs are loaded before any screen, so its width is final here.
   fitHintRow(&hintsBottom, UI_SAFE_RIGHT - CONTENT_X);
   if (uiGetHintsWidth(&hintsBottom) > UI_SAFE_RIGHT - CONTENT_X)
      logWarn("[home] bottom hint row is %d px for %d: it reaches under the rail\n", uiGetHintsWidth(&hintsBottom),
              UI_SAFE_RIGHT - CONTENT_X);

   // The fonts are the shared ones (ui-fonts.h): nothing here opens or closes a font. The title is external
   // text on the playlist tab; its width is set by fitTitleWidth on every heading.
   uiInitTextLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, TITLE_MIN_W, UI_PAGE_TITLE_SIZE,
                   activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&statusLabel,   STATUS_WEIGHT, 0, 0, AUTO, STATUS_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   uiInitLabel(&pageWordLabel, PAGE_WEIGHT,   0, 0, AUTO, PAGE_SIZE,   activeTheme->textSecondary, TEXT_NOWRAP, "Page");
   uiInitLabel(&pagePlusLabel, PAGE_WEIGHT,   0, 0, AUTO, PAGE_SIZE,   activeTheme->textSecondary, TEXT_NOWRAP, "+");
   uiInitDigits(&pageDigits, PAGE_WEIGHT, PAGE_SIZE);
   for (int c = 0; c < TREND_COUNT; c++)
      uiInitLabel(&chipLabels[c], CHIP_WEIGHT, 0, 0, AUTO, CHIP_SIZE, TEXT_TINTABLE, TEXT_NOWRAP, TrendingNames[c]);
   for (int c = 0; c < LIVE_COUNT; c++)
      uiInitLabel(&liveChipLabels[c], CHIP_WEIGHT, 0, 0, AUTO, CHIP_SIZE, TEXT_TINTABLE, TEXT_NOWRAP, LiveNames[c]);
   home.liveChipPad   = CHIP_PAD_X;   // as designed until a Live heading works out what fits (fitLiveChips)
   home.liveChipFirst = 0;
   home.liveChipLast  = LIVE_COUNT - 1;

   home.noticeVisible   = 0;
   home.noticeIsConfirm = 0;
   home.noticePanelW    = home.screenW * 2 / 3;
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, home.noticePanelW - 2 * NOTICE_PAD_X, NOTICE_TEXT,
                   activeTheme->textPrimary, TEXT_WRAP, "");
   // the answers with the console's own glyphs, as in the other boxes; O closes a plain notice too, as ever
   uiInitHints(&closeHints);
   uiAddHint(&closeHints, GLYPH_CROSS, "Close");
   uiInitHints(&confirmHints);
   uiAddHint(&confirmHints, GLYPH_CROSS,  "Unsubscribe");
   uiAddHint(&confirmHints, GLYPH_CIRCLE, "Cancel");
   uiInitHints(&interruptedHints);
   uiAddHint(&interruptedHints, GLYPH_CROSS,    "Restart");
   uiAddHint(&interruptedHints, GLYPH_TRIANGLE, "Delete");
   uiAddHint(&interruptedHints, GLYPH_CIRCLE,   "Later");

   home.pendingReload = 0;

   // Bild 4's cards (GRID_VIEW_GRID, the grid's own default): four columns between the rail and the safe edge
   initVideoGrid(&home.grid, uiGetFont(UI_CARD_TITLE_WEIGHT), CONTENT_X, CARDS_TOP,
                 UI_SAFE_RIGHT - CONTENT_X, GRID_BOTTOM - CARDS_TOP);
   setGridHero(&home.grid, 1);
   pageMaxW = measurePageMaxW();   // needs the grid's page size
   initSidebar();
   setGridWatchedPredicate(&home.grid, tileWatched);
   setGridWatchLaterPredicate(&home.grid, tileInWatchLater);
   setGridProgressSource(&home.grid, getWatchedPosition);

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   historyRevisionSeen = getWatchHistoryRevision();
   playlistRevisionSeen = getPlaylistsRevision();
   regionRevisionSeen = getContentRegionRevision();
   accountRevisionSeen = getAccountRevision();
   accountChangesSeen = getAccountChangesRevision();
   home.forYouAfterSignIn = 0;
   home.category = HOME_SUBS;   // always start on Subscriptions - the account's, while signed in
   loadCategory();
}

// LEFT belongs to the grid while there is a card to the left of the selection, and to the rail from the
// leftmost column - pages are whole rows, so the column is the index modulo the columns. With no cards on
// screen (loading, nothing found) there is nothing for LEFT to move, and it opens the rail as it always did.
static int leftOpensRail(void)
{
   if (gridStage(&home.grid) != GRID_READY || !gridSelected(&home.grid) || home.grid.cols <= 1) return 1;
   // (Test 15) on a later page LEFT off the first card goes back a page (video-grid.c); the rail is page 1's, and O's
   return gridSelectedIndex(&home.grid) % home.grid.cols == 0 && gridPageIndex(&home.grid) == 0;
}

// What a pick in the rail does - from Home's own rail, or handed over by a screen that was over Home
// (updateRailOverScreen). Returns 1 when it loaded another category.
static int actOnRailPick(int picked)
{
   if      (picked == SIDE_SEARCH)    openSearchInput(NULL, onQueryEntered);
   else if (picked == SIDE_PLAYLISTS) openPlaylists(onPlaylistOpened, onAccountPlaylistOpened);
   else if (picked == SIDE_SETTINGS)  openSettingsScreen();
   else if (picked == SIDE_FORYOU && !isAccountSignedIn()) {   // For you needs the account: sign in first
      home.forYouAfterSignIn = 1;
      openAccountScreen(SIDE_FORYOU);
   }
   else if (picked == SIDE_ACCOUNT) {   // signed out the sign-in, signed in the account's page
      if (isAccountSignedIn()) openAccountPage();
      else openAccountScreen(SIDE_ACCOUNT);
   }
   else {
      int category = categoryForItem(picked);
      if (category >= 0 && category != home.category) { cacheCurrent(); home.category = category; loadCategory(); return 1; }
   }
   return 0;
}

// A pick made in the rail of a screen that was over Home, for Home's next update to act on. Not acted on during
// the pop that brings Home back: Home's resume runs inside popScreen, and a screen pushed from there would leave
// the stack in an order the pop loop below does not expect.
static int pendingRailPick = -1;

int updateRailOverScreen(SidebarItem own)
{
   setSidebarCurrent(own);
   if (!isSidebarFocused()) {
      if (!isPadButtonPressed(PAD_BTN_LEFT)) return RAIL_NOT_OPEN;
      setSidebarFocused(1);   // parks the highlight on the screen's own entry
      return RAIL_OPEN;
   }
   int picked = updateSidebar();
   if (picked < 0) return RAIL_OPEN;   // still open, or O or RIGHT just closed it: the frame was the rail's either way
   setSidebarFocused(0);
   if (picked == (int)own) return RAIL_PICKED_OWN;

   // Every screen above Home goes, top first, each with its own term and the next one's resume, exactly as O
   // would take them down one by one. Home is the root of the stack, so the loop stops there; the bound only
   // keeps a stack without Home from spinning.
   pendingRailPick = picked;
   for (int i = 0; i < 16 && homeScreen.status != SCREEN_ACTIVE; i++) popScreen();
   return RAIL_GONE;
}

// The account page's pick, for Home's next update to act on (as pendingRailPick: not during the pop that brings
// Home back).
static int pendingSection = -1;

void showHomeSection(HomeSection section)
{
   pendingSection = section;
   for (int i = 0; i < 16 && homeScreen.status != SCREEN_ACTIVE; i++) popScreen();
}

// 1 when it loaded another page
static int actOnSection(int section)
{
   int category = -1;
   switch (section) {
   case HOME_SECTION_FOR_YOU:       category = HOME_FORYOU;     break;
   case HOME_SECTION_SUBSCRIPTIONS: category = HOME_SUBS;       break;
   case HOME_SECTION_WATCH_LATER:   category = HOME_WATCHLATER; break;
   case HOME_SECTION_HISTORY:       category = HOME_HISTORY;    break;
   case HOME_SECTION_LIKED:         onAccountPlaylistOpened("LL", "Liked videos"); return 1;
   case HOME_SECTION_PLAYLISTS:     openPlaylists(onPlaylistOpened, onAccountPlaylistOpened); return 0;
   default: return 0;
   }
   if (category == home.category) return 0;   // already on it: a stale one still reloads (pendingReload)
   cacheCurrent();
   home.category = category;
   loadCategory();
   return 1;
}

static void updateHome(void)
{
   // (Test 15) once a run, first thing: a version not welcomed yet shows its changes and the support page
   static int welcomeChecked;
   if (!welcomeChecked) {
      welcomeChecked = 1;
      if (isWelcomeDue()) { openWelcome(); return; }
   }
   if (pendingSection >= 0) {   // a pick from the account page (showHomeSection)
      int section = pendingSection;
      pendingSection = -1;
      if (actOnSection(section)) home.pendingReload = 0;
      return;
   }
   if (pendingRailPick >= 0) {   // a pick from the rail of a screen that was over Home (updateRailOverScreen)
      int picked = pendingRailPick;
      pendingRailPick = -1;
      if (actOnRailPick(picked)) home.pendingReload = 0;   // it loaded the new category: nothing stale is left
      return;
   }
   if (home.pendingReload) { home.pendingReload = 0; loadCategory(); }

   // the account's channels and Watch Later, loaded once per sign-in; a change YouTube refused says so here
   syncAccountLists();
   if (!home.noticeVisible) {
      char failure[160];
      if (takeAccountFailure(failure, sizeof failure)) showNotice(failure);
   }
   // (V3.0.0 Test 12) a newer release on GitHub: said once a start, when no other notice is up
   static int updateTold;
   char newer[16];
   if (!updateTold && !home.noticeVisible && getNewerRelease(newer, sizeof newer)) {
      updateTold = 1;
      char message[256];
      snprintf(message, sizeof message, "TEE Vanced v%s is out - this is %s. It is on GitHub: TheErsysEnding/TEE-Vanced-PS3, "
               "Releases.", newer, APP_VERSION);
      showNotice(message);
   }

   updateDownloadOverlay();
   if (oskInputActive()) return;

   // (V3.0.0) once a run, at the first chance: downloads the last run did not finish - restart, delete, or ask later
   if (!home.interruptedChecked && !home.noticeVisible) {
      home.interruptedChecked = 1;
      char title[160];
      int count = findInterruptedDownloads(title, sizeof title);
      if (count > 0) {
         char message[320];
         if (count == 1) snprintf(message, sizeof message, "The download of \"%s\" did not finish. Start it again "
                                  "from the beginning, or delete it?", title);
         else snprintf(message, sizeof message, "%d downloads did not finish (\"%s\" and %d more). Start them again from "
                       "the beginning, or delete them?", count, title, count - 1);
         showNotice(message);
         home.noticeInterrupted = 1;
      }
   }

   if (home.noticeVisible && home.noticeInterrupted) {   // its own answers (interruptedHints)
      if (isPadButtonPressed(PAD_BTN_CROSS))         { home.noticeVisible = home.noticeInterrupted = 0; restartInterruptedDownloads(); }
      else if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { home.noticeVisible = home.noticeInterrupted = 0; deleteInterruptedDownloads(); }
      else if (isPadButtonPressed(PAD_BTN_CIRCLE))   { home.noticeVisible = home.noticeInterrupted = 0; }   // asked again next start
      return;
   }

   if (home.noticeVisible) {   // modal: nothing underneath sees the pad until it is dismissed
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         home.noticeVisible = 0;
         // clear the flag BEFORE acting - the unsubscribe may put an error box up in its place
         if (home.noticeIsConfirm) {
            home.noticeIsConfirm = 0;
            if (pendingDeleteId[0]) {   // (V3.0.0) a download, not a channel
               deleteDownload(pendingDeleteId);
               pendingDeleteId[0] = 0;
               downloadsRevisionSeen = getDownloadsRevision();
               if (home.category == HOME_DOWNLOADS) {
                  int at = gridSelectedIndex(&home.grid);
                  loadCategory();
                  keepGridSelection(&home.grid, at);   // (Test 15)
               }
            } else if (pendingMoveId[0]) {   // into the XMB: the page loses it once the XMB has it (revision)
               moveDownloadToXmb(pendingMoveId);
               pendingMoveId[0] = 0;
            } else if (pendingCancelId[0]) {   // (V3.0.0) stopped: the page re-reads the list (revision)
               cancelDownload(pendingCancelId);
               pendingCancelId[0] = 0;
            } else {
               unsubscribeConfirmed();
            }
         }
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { home.noticeVisible = 0; home.noticeIsConfirm = 0; pendingDeleteId[0] = 0; pendingMoveId[0] = 0;
                                                pendingCancelId[0] = 0; }
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
         setSidebarFocused(0);
         actOnRailPick(picked);
      }
      if (!isSidebarFocused()) setGridInputSuppressed(&home.grid, 0);
      return;
   }
   // O opens it from anywhere. LEFT opens it only from the leftmost column of cards - or when there are no
   // cards to move between: elsewhere LEFT moves the selection left, as a grid needs it to.
   if ((isPadButtonPressed(PAD_BTN_LEFT) && leftOpensRail()) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
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

   int pageBefore = gridPageIndex(&home.grid);
   updateVideoGrid(&home.grid);
   // one line per page turn in dbg.txt: VRAM has to come back to the same level page after page
   if (gridPageIndex(&home.grid) != pageBefore) uiLogVram("home page turn");
   if (gridStage(&home.grid) != GRID_READY) return;

   // the feed on screen now reflects these revisions - only at this point is a change really "seen"
   if (!gridBusy(&home.grid)) {
      if (home.category == HOME_SUBS)       subsRevisionSeen = getSubscriptionsRevision();
      if (home.category == HOME_WATCHLATER) watchLaterRevisionSeen = getWatchLaterRevision();
      if (home.category == HOME_HISTORY ||
          home.category == HOME_CONTINUE)   historyRevisionSeen = getWatchHistoryRevision();
      if (home.category == HOME_PLAYLIST)   playlistRevisionSeen = getPlaylistsRevision();
   }

   if (home.category == HOME_DOWNLOADS && getDownloadsRevision() != downloadsRevisionSeen && !gridBusy(&home.grid)) {
      loadCategory();   // a download landed (or went) while the page was up
      return;
   }

   const SearchResult *selected = gridSelected(&home.grid);
   // (V3.0.0) on the Downloads page the hints follow the selection: a download on its way can only be stopped
   static int pendingHints = -1;
   int wantPending = home.category == HOME_DOWNLOADS ? (selected && selected->downloadState) : -1;
   if (wantPending != pendingHints) {
      pendingHints = wantPending;
      if (wantPending >= 0) {
         uiSetHintShown(&hintsTop, CROSS_HINT, !wantPending);
         uiSetHintShown(&hintsTop, SQUARE_HINT, !wantPending);
         uiSetHintCaption(&hintsTop, TRIANGLE_HINT, wantPending ? "Stop download" : "Delete");
      } else {
         uiSetHintShown(&hintsTop, CROSS_HINT, 1);
         uiSetHintShown(&hintsTop, SQUARE_HINT, !subsAreChannels());
      }
      fitTitleWidth();
   }
   if (!selected) return;
   if (home.category == HOME_DOWNLOADS && selected->downloadState) {   // (V3.0.0) on its way: Triangle stops it
      if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
         char message[256];
         snprintf(message, sizeof message, "Stop downloading \"%s\"? What is in so far is thrown away.", selected->title);
         strCopy(pendingCancelId, sizeof pendingCancelId, selected->videoId);
         showNotice(message);
         uiSetHintCaption(&confirmHints, 0, "Stop download");
         home.noticeIsConfirm = 1;
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         char message[256];
         snprintf(message, sizeof message, selected->downloadState == 1
                  ? "\"%s\" is still downloading. It plays here once it is in."
                  : "\"%s\" waits for the downloads before it. It plays here once it is in.", selected->title);
         showNotice(message);
         return;
      }
      if (isPadButtonPressed(PAD_BTN_SQUARE) || isPadButtonPressed(PAD_BTN_R3)) return;
   }
   if (home.category == HOME_DOWNLOADS) {   // (V3.0.0) X plays it from disk, Triangle deletes it
      if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
         char message[256];
         snprintf(message, sizeof message, "Delete the download \"%s\" from the console?", selected->title);
         strCopy(pendingDeleteId, sizeof pendingDeleteId, selected->videoId);
         showNotice(message);
         uiSetHintCaption(&confirmHints, 0, "Delete");
         home.noticeIsConfirm = 1;
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         char input[48];
         snprintf(input, sizeof input, "%s%s", LOCAL_INPUT_PREFIX, selected->videoId);
         stopVideoGrid(&home.grid);
         playVideo(input);
         return;
      }
      if (isPadButtonPressed(PAD_BTN_R3)) return;   // already downloaded
      if (isPadButtonPressed(PAD_BTN_SQUARE)) {     // into the XMB's Video column, after the warning
         char message[320];
         snprintf(message, sizeof message, "Move \"%s\" into the XMB's Video column? The system takes the file: TEE "
                  "Vanced will not list or play it afterwards. The XMB plays it at 720p60 or 1080p30 (a 1440p or "
                  "1080p60 download is fetched again in that form).", selected->title);
         strCopy(pendingMoveId, sizeof pendingMoveId, selected->videoId);
         showNotice(message);
         uiSetHintCaption(&confirmHints, 0, "Move to the XMB");
         home.noticeIsConfirm = 1;
         return;
      }
   }
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      if (subsAreChannels()) return;   // (V3.0.0) a channel's tile has no video to keep for later
      if (isAccountSignedIn()) {   // the account's Watch Later: the tag flips at once, YouTube is told on a worker
         int inList = isInAccountWatchLater(selected->videoId);
         char videoId[sizeof selected->videoId];
         strCopy(videoId, sizeof videoId, selected->videoId);
         setAccountWatchLater(videoId, !inList);
         accountChangesSeen = getAccountChangesRevision();   // handled here, not by a reload on the way back
         if (inList && home.category == HOME_WATCHLATER) dropFromPage(videoId);
         else categoryCached[HOME_WATCHLATER] = 0;
         return;
      }
      toggleWatchLater(selected);
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) {   // reflect the add/remove immediately, where the selection was
         int at = gridSelectedIndex(&home.grid);
         loadCategory();
         keepGridSelection(&home.grid, at);   // (Test 15)
      }
      return;
   }
   // live can't be downloaded: on a Live page R3 does nothing at all (its hint is hidden there, setHeading)
   if (isPadButtonPressed(PAD_BTN_R3)) {
      if (!isLiveCategory(home.category) && !selected->isLive) openDownloadChoice(selected);
      return;
   }
   // L3 came free when the grid layout did. Filing a video is the thing you want a direct button for -
   // everything else about a playlist happens on its own screen.
   if (isPadButtonPressed(PAD_BTN_L3) && !subsAreChannels()) { openPlaylistChooser(selected); return; }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      // In Subscriptions a tile IS a channel and X already opens it (just below), so Triangle was doing the
      // exact same thing as X here. It is also the button that unsubscribes one level down, inside the
      // channel screen - so this keeps the meaning and only drops the two presses it took to get there.
      if (subsAreChannels()) { askUnsubscribe(selected); return; }
      openSelectedChannel(selected);   // a tile without a channel id (For you) is left alone by openChannelFor
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      // in the Subscriptions tab a tile is a channel, so X opens it instead of playing anything
      if (subsAreChannels()) { openSelectedChannel(selected); return; }
      markWatchedItem(selected);   // full entry, so History can list it
      stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
      // hand over the whole list so autoplay can carry on with what follows
      playVideoFromList(gridResults(&home.grid), gridSelectedIndex(&home.grid));
   }
}

// The modal box, laid out from its text's own measurements, so a message in FONT_POP (a Japanese channel
// name) sits exactly where a Manrope one would: the message's capitals NOTICE_PAD_TOP below the top edge, the
// answers' glyph row NOTICE_ANSWERS_DY below the message's last baseline, and NOTICE_PAD_BOTTOM under it.
static void drawNotice(void)
{
   UiTextMetrics text = uiGetTextMetrics(noticeLabel.font, noticeLabel.size);
   const UiHints *answers = home.noticeInterrupted ? &interruptedHints : home.noticeIsConfirm ? &confirmHints : &closeHints;
   int textTop      = NOTICE_PAD_TOP - text.capTop;                                     // panel-relative
   int lastBaseline = textTop + noticeLabel.tt.tex.h - text.texH + text.baseline;      // one pitch a line
   int answersTop   = lastBaseline + NOTICE_ANSWERS_DY;
   int panelW = home.noticePanelW, panelH = answersTop + UI_HINT_GLYPH_H + NOTICE_PAD_BOTTOM;
   int x = (home.screenW - panelW) / 2, y = (home.screenH - panelH) / 2;

   uiFillRect(0, 0, home.screenW, home.screenH, activeTheme->scrim);
   uiFillRounded(x, y, panelW, panelH, NOTICE_RADIUS, activeTheme->surface);
   uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);   // the box owns the pad
   uiDrawLabelAt(&noticeLabel, x + NOTICE_PAD_X, y + textTop);
   uiDrawHints(answers, x + NOTICE_PAD_X, y + answersTop, UI_ALIGN_LEFT);
}

// a label rendered in TEXT_TINTABLE, drawn at (x, y) in `color` (an opaque theme colour)
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

// A chip after the title (Bild 4's): the one on screen in text-1 with surface-1 text, the others on surface-2 in
// text-2, padX either side of the word. Returns its width.
static int drawChip(const Label *label, int x, int padX, int on)
{
   int w = label->tt.tex.w + 2 * padX;
   uiFillPill(x, HEADER_Y, w, HEADER_H, on ? activeTheme->textPrimary : activeTheme->surface);
   drawTinted(label, x + padX, uiLabelYForCenter(label, HEADER_Y, HEADER_H),
              on ? activeTheme->surface1 : activeTheme->textSecondary);
   return w;
}

// Trending's categories after its title. L1/R1 still walk all eight categories, not just these three. A chip
// that would reach the top hint row is left out rather than drawn into it.
static void drawTrendingChips(int x)
{
   int limit = hintsTopLeft() - TITLE_GAP;
   for (int c = 0; c < TREND_COUNT; c++) {
      const Label *label = &chipLabels[c];
      if (label->tt.tex.w <= 0 || x + label->tt.tex.w + 2 * CHIP_PAD_X > limit) break;
      x += drawChip(label, x, CHIP_PAD_X, c == home.category) + CHIP_GAP;
   }
}

// Live's six chips after its title, L1/R1's ring on this page: the run fitLiveChips found room for, which
// always holds the one on screen.
static void drawLiveChips(int x)
{
   int active = home.category - HOME_LIVE_FIRST;
   for (int c = home.liveChipFirst; c <= home.liveChipLast; c++) {
      if (liveChipLabels[c].tt.tex.w <= 0) continue;
      x += drawChip(&liveChipLabels[c], x, home.liveChipPad, c == active) + CHIP_GAP;
   }
}

// "Page 3 / 12+" with its right edge at `right`: the word and the "+" are labels, the numbers equal-width
// digits, so turning a page re-renders nothing and the line does not wobble.
static void drawPageCounter(int right)
{
   char numbers[32];
   snprintf(numbers, sizeof numbers, " %d / %d", gridPageIndex(&home.grid) + 1, gridPageCount(&home.grid));
   int more     = gridHasMorePages(&home.grid);
   int numbersW = uiGetDigitsWidth(&pageDigits, numbers);
   int x = right - (more ? pagePlusLabel.tt.tex.w : 0) - numbersW - pageWordLabel.tt.tex.w;
   int digitsY  = uiTextYForCenter(pageDigits.font, pageDigits.size, HEADER_Y, HEADER_H);
   int baseline = digitsY + uiGetTextMetrics(pageDigits.font, pageDigits.size).baseline;
   uiDrawLabelAt(&pageWordLabel, x, uiLabelYForBaseline(&pageWordLabel, baseline));
   x += pageWordLabel.tt.tex.w;
   uiDrawDigits(&pageDigits, x, digitsY, numbers, activeTheme->textSecondary);
   if (more) uiDrawLabelAt(&pagePlusLabel, x + numbersW, uiLabelYForBaseline(&pagePlusLabel, baseline));
}

// the header line: the title (and Trending's or Live's chips), the page counter and the clock - the hints come
// later
static void drawHeader(void)
{
   uiDrawLabelAt(&titleLabel, CONTENT_X, uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H));
   int chipsX = CONTENT_X + titleLabel.tt.tex.w + CHIP_TITLE_GAP;
   if      (home.category < TREND_COUNT)     drawTrendingChips(chipsX);
   else if (isLiveCategory(home.category))   drawLiveChips(chipsX);
   if (gridStage(&home.grid) == GRID_READY) drawPageCounter(pageRight());

   Font *clockFont = uiGetFont(UI_WEIGHT_BOLD);
   int clockY = uiTextYForCenter(clockFont, CLOCK_SIZE, HEADER_Y, HEADER_H);
   uiDrawClock(UI_SAFE_RIGHT, clockY + uiGetTextMetrics(clockFont, CLOCK_SIZE).baseline);
}

static void drawHome(void)
{
   fillGfxRectangle(0, 0, home.screenW, home.screenH, activeTheme->appBg);

   // Order matters. The grid goes down first, then the header (after the grid, because the selection's glow
   // reaches up under it), the hint rows and the download pill, then the rail. Expanded, the rail lies OVER
   // the page - header included, as the design has it - and dims everything right of it with its scrim,
   // rather than pushing anything aside: the title starts at the cards' left edge, inside the expanded panel,
   // so drawn after the rail the 46-px title ran straight across the open menu. The hints and the pill are the
   // page's too: while the rail has the pad none of those buttons does what its hint says, and drawn over the
   // scrim they looked as live as the menu. Only a notice goes on top of everything.
   if (gridStage(&home.grid) == GRID_READY) {
      drawGridHero(&home.grid);
      drawVideoGrid(&home.grid);
   } else {
      // where the cards will be, clear of the collapsed rail; under the panel while the menu is open
      setLabelText(&statusLabel, gridStage(&home.grid) != GRID_EMPTY ? "Loading..." : home.category == HOME_DOWNLOADS
                                 ? "No downloads yet - R3 on any video downloads it." : "No results");
      uiDrawLabelAt(&statusLabel, CONTENT_X, uiLabelYForBaseline(&statusLabel, STATUS_BASELINE));
   }
   drawHeader();
   uiDrawHints(&hintsTop,    hintsTopRight(), HEADER_HINTS_Y,    UI_ALIGN_RIGHT);
   uiDrawHints(&hintsBottom, UI_SAFE_RIGHT,   UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   // (V3.0.0) a download is in, or failed: a few seconds under the top hints, right, clear of the hero's title
   drawDownloadResultAt(UI_SAFE_RIGHT, HEADER_HINTS_Y + UI_HINT_GLYPH_H + 12, UI_ALIGN_RIGHT);
   // (V3.0.0) the download shows in the rail now (sidebar.c): the old chip bottom right would lie over the cards

   drawSidebar(home.screenH);

   if (home.noticeVisible) drawNotice();
}

static void suspendHome(void) { stopVideoGrid(&home.grid); }   // free the http pools for the pushed screen

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   // A direction still held from the screen that just closed must not drive the list now: the player's UP/DOWN
   // (volume) or LEFT/RIGHT (seek), held through the O that left it, found the grid's repeat timers stale from
   // before the push - long overdue, so the selection moved on the first frame back and then raced through
   // the pages. Fresh timers make a held button wait the normal hold delay; a new press still acts at once.
   resetGridRepeat(&home.grid);

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
   int playlistStale = getPlaylistsRevision() != playlistRevisionSeen;
   if (playlistStale) categoryCached[HOME_PLAYLIST] = 0;

   // A new content region (Settings > Region) makes every list YouTube chose for the old one stale: the three
   // Trending feeds are dropped, a Trending or Live page on screen reloads (Live is never cached). Unlike the
   // markers above, this one moves on at once - every regional cache goes in the same breath and the page on
   // screen reloads whatever happens, so there is no retry to lose. Subscriptions stay: a channel's uploads are
   // the same in every region, and a refetch costs one request per channel. Continue watching, Watch Later,
   // History and the playlists are kept on the console.
   int regionStale = getContentRegionRevision() != regionRevisionSeen;
   if (regionStale) {
      regionRevisionSeen = getContentRegionRevision();
      for (int c = 0; c < TREND_COUNT; c++) categoryCached[c] = 0;
      categoryCached[HOME_FORYOU] = 0;
      logInfo("[home] region now %s: Trending and Live reload\n", getContentRegionCode());
   }

   // A sign-in or a sign-out changes where Subscriptions, Watch Later and History come from, and whether For you
   // has anything at all: their copies go, and such a page on screen reloads. Signed out on For you, the page
   // becomes Subscriptions; signed in after picking For you in the rail, it becomes For you. Moved on at once, as
   // the region's marker is.
   // A change made with Square or Triangle somewhere else - on a channel page, in Search - leaves Watch Later and the
   // subscription feed behind: their copies go, and either page on screen reloads.
   int changesStale = getAccountChangesRevision() != accountChangesSeen;
   if (changesStale) {
      accountChangesSeen = getAccountChangesRevision();
      categoryCached[HOME_WATCHLATER] = categoryCached[HOME_SUBS] = 0;
   }

   int accountStale = getAccountRevision() != accountRevisionSeen;
   if (accountStale) {
      accountRevisionSeen = getAccountRevision();
      categoryCached[HOME_SUBS] = categoryCached[HOME_WATCHLATER] = categoryCached[HOME_HISTORY] = 0;
      categoryCached[HOME_FORYOU] = 0;
      if (home.category == HOME_FORYOU && !isAccountSignedIn()) home.category = HOME_SUBS;
      if (home.forYouAfterSignIn && isAccountSignedIn()) home.category = HOME_FORYOU;
      if (playlistIsAccount && !isAccountSignedIn()) {   // a YouTube playlist is gone with the sign-in
         playlistIsAccount = 0;
         categoryCached[HOME_PLAYLIST] = 0;
         if (home.category == HOME_PLAYLIST) home.category = HOME_SUBS;
      }
      logInfo("[home] %s: the account's pages reload\n", isAccountSignedIn() ? "signed in" : "signed out");
   }

   // A Live page lists what is on air NOW, and streams end: back after ten minutes - a stream watched for a
   // while, say - a good part of it would show LIVE on recordings and on streams that no longer play. Such a
   // page is fetched again; a younger one keeps its list and its selection, like every other page.
   int liveStale = 0;
   if (isLiveCategory(home.category) && gridStage(&home.grid) == GRID_READY) {
      uint64_t age = sys_time_get_system_time() - home.liveLoadedUs;
      if (age >= LIVE_MAX_AGE_US) {
         liveStale = 1;
         logInfo("[home] Live page is %d min old: it reloads\n", (int)(age / 60000000ull));
      }
   }

   home.forYouAfterSignIn = 0;   // whatever came of it: the next resume is not about that sign-in

   int showingStale = (subsStale && home.category == HOME_SUBS) ||
                      (laterStale && home.category == HOME_WATCHLATER) ||
                      (historyStale && (home.category == HOME_HISTORY || home.category == HOME_CONTINUE)) ||
                      (playlistStale && home.category == HOME_PLAYLIST) ||
                      (regionStale && (home.category < TREND_COUNT || isLiveCategory(home.category) ||
                                       home.category == HOME_FORYOU)) ||
                      (changesStale && isAccountSignedIn() &&
                       (home.category == HOME_SUBS || home.category == HOME_WATCHLATER)) ||
                      (accountStale && (home.category == HOME_SUBS || home.category == HOME_WATCHLATER ||
                                        home.category == HOME_HISTORY || home.category == HOME_FORYOU)) ||
                      liveStale;

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
   uiTermHints(&hintsTop);
   uiTermHints(&hintsBottom);
   freeLabel(&titleLabel);
   freeLabel(&statusLabel);
   freeLabel(&pageWordLabel);
   freeLabel(&pagePlusLabel);
   uiFreeDigits(&pageDigits);
   for (int c = 0; c < TREND_COUNT; c++) freeLabel(&chipLabels[c]);
   for (int c = 0; c < LIVE_COUNT; c++) freeLabel(&liveChipLabels[c]);
   freeLabel(&noticeLabel);
   uiTermHints(&closeHints);
   uiTermHints(&confirmHints);
   uiTermHints(&interruptedHints);
   // no closeFont: the fonts are shared (ui-fonts.h), and closing one would blank every label that uses it
}

void openHome(void) { changeScreen(&homeScreen); }

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };

// search screen - a VideoGrid in one of two modes: a text search or a channel's videos (see search.h).
// Pushed on top of home. Triangle drills into a channel in place, Circle backs out (channel -> search
// results -> pop to home), SELECT cycles the sort, Start re-searches, X plays.
//
// The page is Bild 5 (Ember): the collapsed rail at the left edge, drawn passive (Circle is Back on this
// pushed screen, so the rail only says where you are); the query - or the channel's name - in the search
// field's look, read-only, since Start opens the real field (search-input.c); the page count where Bild 5
// has its result count; the sort as a segment under the field, a running download's chip at that row's left
// end; then the results, three rows to a page.

#include "screens/search.h"
#include "screens/search-input.h"
#include "screens/download-choice.h"   // R3: in which form to download
#include "screens/playlist-screen.h"
#include "screens/play.h"
#include "screens/home.h"       // the rail over this screen (updateRailOverScreen)
#include "account.h"            // signed in: Square and Triangle act on the YouTube account
#include "video-grid.h"
#include "storage.h"
#include "downloads.h"
#include "sidebar.h"            // the rail: collapsed, or open over the screen while it has the focus
#include "ui-hints.h"           // the Ember hint row
#include "ui-draw.h"            // safe area, rounded fills, focus ring, fixed-width digits, uiDrawLabelAt
#include "ui-fonts.h"           // the shared fonts, the type scale, label helpers and text placement
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"       // the field's magnifier

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

// Content runs from beside the collapsed rail (112 px wide) to the safe area's right edge - x 176..1824,
// the column every Ember page uses.
#define CONTENT_X         SIDEBAR_CONTENT_X
#define CONTENT_RIGHT     UI_SAFE_RIGHT
#define CONTENT_W         (CONTENT_RIGHT - CONTENT_X)
// The search field as Bild 5 draws it, here showing what the results are for: 88 tall at the top of the
// page, radius 26 with a 2-px inner border; the magnifier's 34-px box 30 in from the left, the text 88 in
// (30 + 34 + 22 + the input's 2), and the page count ending 30 in from the right.
#define FIELD_Y           58
#define FIELD_H           88
#define FIELD_RADIUS      26
#define FIELD_BORDER      2       // its inner border: hairline over the field colour
#define FIELD_PAD         30
#define FIELD_ICON_BOX    34
#define FIELD_ICON_SIZE   28      // the icon font's magnifier at 28 px is about 28 px of ink, as drawn in Bild 5
#define FIELD_TEXT_X      (CONTENT_X + 88)
#define FIELD_TEXT        34
#define FIELD_WEIGHT      UI_WEIGHT_BOLD
#define FIELD_TEXT_GAP    24      // the query's end ("...") -> the page count
// The page count takes the field's right end, where Bild 5 shows "24 results" in 20/600 text-3: a feed never
// states its total, so that number is not known, but the page count is.
#define PAGE_SIZE         UI_META_SIZE
#define PAGE_WEIGHT       UI_META_WEIGHT
// The sort, under the field and right-aligned as in Bild 5: a surface-1 group (padding 5, gap 4, radius 16)
// of options 45 tall (radius 12, 20 px either side of 20/700 text). It only shows the sort - SELECT changes
// it - so the one in force is simply the solid accent with onAccent text, the others textSecondary. It sits
// 20 under the field where Bild 5 has 26: the 6 px go to the three result rows below.
#define SEGMENT_Y         (FIELD_Y + FIELD_H + 20)
#define SEGMENT_H         55
#define SEGMENT_PAD       5
#define SEGMENT_GAP       4
#define SEGMENT_RADIUS    16
#define OPTION_H          (SEGMENT_H - 2 * SEGMENT_PAD)
#define OPTION_RADIUS     12
#define OPTION_PAD_X      20
#define OPTION_SIZE       20
#define OPTION_WEIGHT     UI_WEIGHT_BOLD
// A running download's chip (downloads.c) sits at the left end of the segment's row, centred on it - where
// Bild 5 has its recent-search chips, which this screen does without. Its default spot above the hint row
// lies across the third row and its focus ring (down to y 962): the rows run to just above the hint row.
// Here it keeps 22 px from the first row's ring and always fits beside the segment: downloads.c ends its
// text in "..." at 560 px, which makes the chip at most about 750 px wide, and the row has over 1300 px left
// of the widest segment (Latest / Popular / Oldest, 342 px).
#define DOWNLOAD_CHIP_Y   (SEGMENT_Y + (SEGMENT_H - DOWNLOAD_OVERLAY_H) / 2)
// The list's rectangle: from just under the segment to just above the hint row, x 176..1824, 731 px tall -
// the list view lays three rows of about 230 into it. A focused row's ring stands 8 px out of its plate:
// above the first row it stays 16 px clear of the segment, below the last 14 px clear of the hint glyphs.
#define GRID_TOP          (SEGMENT_Y + SEGMENT_H + 24)
#define GRID_BOTTOM       (UI_HINTS_BOTTOM_Y - 22)   // the list's rectangle ends above the hint row; until v0.13
                                                     // it ran to the bottom edge, under the hints
#define STATUS_SIZE       24
#define STATUS_WEIGHT     UI_WEIGHT_BOLD
#define STATUS_BASELINE   (GRID_TOP + 40)
#define NOTICE_PAD_X      44        // modal confirmation box: inner padding left and right
#define NOTICE_PAD_TOP    40        //   panel top -> the first capitals of the message
#define NOTICE_PAD_BOTTOM 30        //   the hint line -> panel bottom
#define NOTICE_GAP        24        //   the message -> the hint line
#define NOTICE_TEXT       24
#define NOTICE_HINT_H     UI_HINT_GLYPH_H   // the hint line is a one-hint row: as tall as its glyph
#define NOTICE_RADIUS     26        // the search rows' radius - the one the row ring (UI_RING_ROW) is made for
// The hint row's slots (see initSearch). X is retitled while the field has the focus, and the hints of the
// buttons that act on a result are hidden then; Triangle is also retitled per mode.
#define CROSS_HINT        0
#define SQUARE_HINT       2
#define TRIANGLE_HINT     3
#define R3_HINT           5
#define L3_HINT           6
#define L2_HINT           8
#define R2_HINT           9
#define SORT_OPTIONS_MAX  (CHANNEL_SORT_COUNT > SORT_COUNT ? CHANNEL_SORT_COUNT : SORT_COUNT)
// A channel page's Subscribe button: a pill on the sort segment's row, just left of it - the solid accent with
// "Subscribe", or surface-3 with a check and "Subscribed". UP from the first video reaches it (UP again the field),
// X toggles; Triangle still does from anywhere on the page.
#define BUTTON_GAP        16      // the button -> the sort segment
#define BUTTON_PAD_X      26
#define BUTTON_TEXT       20
#define BUTTON_ICON       22
#define BUTTON_ICON_GAP   10

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
   int       sortCount;          // options in the segment: the current mode's sorts
   int       screenW, screenH;
   int       noticeVisible;      // a modal confirmation is up and owns the pad
   int       noticePanelW;
   int       fieldFocused;       // the field at the top has the focus, not the list (UP on the first result)
   int       buttonFocused;      // a channel page's Subscribe button has it (UP on the first video)
} search;

static Label    fieldLabel, statusLabel, pageWordLabel, pagePlusLabel;
static Label    sortLabels[SORT_OPTIONS_MAX];
static Label    noticeLabel;
static UiDigits pageDigits;
static Label    subscribeLabels[2];   // "Subscribe" [0] in on-accent, "Subscribed" [1] in text-1
static Icon     subscribedIcon;       // the check in front of "Subscribed"
static Icon     fieldIcon;
static UiHints  hints, noticeHints;

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

// The hint row has to fit between the content's left edge (beside the rail) and the safe right edge, and
// much of its width is the console's own glyph art, whose size is only known at run time - RPCS3's is far
// wider than the console's (SELECT alone 111 px against 34). Should the art come out wider still, the gaps
// narrow instead of the row running into the rail; at both known sizes the full gap fits.
static void fitHintRow(UiHints *row)
{
   int gap = UI_HINT_GAP;
   uiSetHintsGap(row, gap);
   while (uiGetHintsWidth(row) > CONTENT_RIGHT - CONTENT_X && gap > UI_HINT_GAP / 2) {
      gap -= 2;
      uiSetHintsGap(row, gap);
   }
}

// Followed: by the YouTube account while signed in (account.h), by the console's own list otherwise.
static int followsChannel(const char *channelId)
{
   return isAccountSignedIn() ? isAccountFollowing(channelId) : isSubscribed(channelId);
}

// the Watch Later tag on a result, from the same list Square changes
static int tileInWatchLater(const char *videoId)
{
   return isAccountSignedIn() ? isInAccountWatchLater(videoId) : isWatchLater(videoId);
}

// in channel mode Triangle toggles the subscription (you're already in the channel); elsewhere it drills in.
static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = followsChannel(search.channelId) ? "Unsubscribe" : "Subscribe";
   uiSetHintCaption(&hints, TRIANGLE_HINT, caption);
   fitHintRow(&hints);   // the row's width follows its captions
}

// New text and colour for a label of fixed text with at most one rasterisation, and none when neither
// changed: emptied first, the colour is only stored, and setting the text then renders it once, in that colour.
static void setFixedLabel(Label *l, const char *text, uint32_t color)
{
   if (l->color == color && strncmp(l->text, text, LABEL_MAX_TEXT) == 0) return;
   freeLabel(l);
   setLabelColor(l, color);
   setLabelText(l, text);
}

// How much of text[0..length) is whole characters: a cut in the middle of a character's UTF-8 bytes leaves
// a lead byte whose sequence runs past the end, and that character is dropped.
static int wholeCharsLength(const char *text, int length)
{
   int start = length;
   while (start > 0 && ((unsigned char)text[start - 1] & 0xC0) == 0x80) start--;   // back over continuation bytes
   if (start == 0) return length;                                                 // no lead byte at all: leave it
   unsigned char lead = (unsigned char)text[start - 1];
   int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
   return start - 1 + need <= length ? length : start - 1;
}

// The extractor copies a name into a fixed buffer and stops one byte short of its end, so a name that fills
// its buffer of textCap bytes was cut there - "The Colin Cowherd Podcast and The Herd with Col" - and shown
// as it is it reads as complete. Such a name ends in "..." (three periods: there is no ellipsis glyph),
// after its last whole character and without the space the cut may have left before it.
static void copyMarkingCut(char *out, int cap, const char *text, int textCap)
{
   int length = (int)strlen(text);
   if (length < textCap - 1 || cap < 4) { strCopy(out, cap, text); return; }
   if (length > cap - 4) length = cap - 4;   // room for "..." and the terminator
   length = wholeCharsLength(text, length);
   while (length > 0 && text[length - 1] == ' ') length--;
   memcpy(out, text, length);
   memcpy(out + length, "...", 4);
}

// The segment shows the current mode's sorts, the one in force in onAccent (it sits on the accent) and the
// others in textSecondary. SELECT reaches every one of them - a channel's too, whether or not its feed
// handed over a token for it (cycleChannelSort) - so none is drawn as out of reach. A label renders only
// when its text or colour changes.
static void setSortLabels(void)
{
   int channel = search.channelId[0] != 0;
   const char **names = channel ? ChannelSortNames : SortNames;
   int current = channel ? search.channelSort : search.sort;

   search.sortCount = channel ? CHANNEL_SORT_COUNT : SORT_COUNT;
   for (int i = 0; i < search.sortCount; i++)
      setFixedLabel(&sortLabels[i], names[i], i == current ? activeTheme->onAccent : activeTheme->textSecondary);
}

// The field shows what the results are for: the query, or the channel's name; the segment the mode's sorts
// with the one in force; the Triangle hint what Triangle does in this mode. Every change of mode or sort
// comes through here (reload).
static void setHeading(void)
{
   // the channel's name comes from a result's author field, as large as channelName; the query is shown as
   // it is, being exactly what was searched for
   char channel[sizeof search.channelName + 3];
   copyMarkingCut(channel, sizeof channel, search.channelName, (int)sizeof search.channelName);
   // a channel name or a query: drawn raw, and all of it in FONT_POP when Manrope lacks one of its characters
   uiSetTextLabel(&fieldLabel, FIELD_WEIGHT, search.channelId[0] ? channel : search.query);
   setSortLabels();
   updateTriangleHint();
}

static void setHeaderFocus(int field, int button);

static void toggleSubscription(void)
{
   // the account: the hint flips at once; YouTube is told on a worker, and a refusal comes back as a notice
   if (isAccountSignedIn()) {
      setAccountFollowing(search.channelId, !isAccountFollowing(search.channelId));
      updateTriangleHint();
      if (search.buttonFocused) setHeaderFocus(0, 1);   // X's word on the button follows
      return;
   }
   int wantOn = !isSubscribed(search.channelId);
   int rc = setSubscribed(search.channelId, search.channelName, wantOn);

   updateTriangleHint();
   if (search.buttonFocused) setHeaderFocus(0, 1);
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

// SELECT in channel mode: nothing until the feed has handed over the sort chips (the Latest chip's token is
// the sign), then always the next of Latest -> Popular -> Oldest. A sort's first page is the token its chip
// carries (SearchResults.sortTokens); a sort the feed gave none for loads the plain first page (searchFetch),
// which is the newest uploads.
static void cycleChannelSort(void)
{
   const SearchResults *results = gridResults(&search.grid);
   if (!results->sortTokens[CHANNEL_SORT_LATEST][0]) return;   // chips not loaded yet
   search.channelSort = (search.channelSort + 1) % CHANNEL_SORT_COUNT;
   strCopy(search.channelSortToken, sizeof search.channelSortToken, results->sortTokens[search.channelSort]);
   reload();
}

// The field at the top takes the focus from the list (UP on the first result) and gives it back (DOWN, or a new
// search). While it has it, the list's own focus is not drawn and the pad does not reach the list, and the hints
// of the buttons that act on a result are hidden - they would do nothing.
// The header's focus - the field, or a channel page's Subscribe button - or none, the list's. Either one in the
// header mutes the list and hides the hints of the buttons that act on a result; X says what it does there.
static void setHeaderFocus(int field, int button)
{
   search.fieldFocused  = field;
   search.buttonFocused = button;
   int focused = field || button;
   setGridInputSuppressed(&search.grid, focused || isSidebarFocused());
   setGridFocusHidden(&search.grid, focused);
   uiSetHintCaption(&hints, CROSS_HINT, field ? "Type" : button ? (followsChannel(search.channelId) ? "Unsubscribe" : "Subscribe") : "Play");
   uiSetHintShown(&hints, SQUARE_HINT, !focused);
   uiSetHintShown(&hints, TRIANGLE_HINT, !focused);
   uiSetHintShown(&hints, R3_HINT, !focused);
   uiSetHintShown(&hints, L3_HINT, !focused);
   uiSetHintShown(&hints, L2_HINT, !focused);
   uiSetHintShown(&hints, R2_HINT, !focused);
   fitHintRow(&hints);
}

static void setFieldFocused(int focused)  { setHeaderFocus(focused, 0); }
static void setButtonFocused(int focused) { setHeaderFocus(0, focused); }

static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (search.fieldFocused) setFieldFocused(0);   // the new results get the focus
   if (strstr(text, "youtu")) { stopVideoGrid(&search.grid); playVideo(text); return; }
   search.channelId[0] = 0;   // a search exits channel mode
   strCopy(search.query, sizeof search.query, text);
   search.sort = SORT_RELEVANCE;
   reload();
}

// The keyboard opened from the field (X while it has the focus): a confirmed query goes into the history, as one
// typed on the search field screen does, and runs here in place. Cancelled, nothing changes.
static void onFieldTyped(const char *text)
{
   if (!text || !text[0]) return;
   addSearchHistory(text);
   onQueryEntered(text);
}

// The widest the page count can get, so the field's text can end in "..." short of it. There are at most
// 500 results, so never more than three digits a number, and the digits are drawn fixed-width (UiDigits):
// every three-digit number is as wide as "888".
static int pageCountReserve(void)
{
   return pageWordLabel.tt.tex.w + uiGetDigitsWidth(&pageDigits, " 888 / 888") + pagePlusLabel.tt.tex.w;
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
   // when they are drawn, by their measured capitals, since the field's text may come out in FONT_POP.
   // "Page" and the "+" are labels, the numbers between them fixed-width digits: a count that grows as
   // pages arrive would otherwise shift the whole readout sideways with every digit that changes width.
   uiInitLabel(&pageWordLabel, PAGE_WEIGHT, 0, 0, AUTO, PAGE_SIZE, activeTheme->textTertiary, TEXT_NOWRAP, "Page");
   uiInitLabel(&pagePlusLabel, PAGE_WEIGHT, 0, 0, AUTO, PAGE_SIZE, activeTheme->textTertiary, TEXT_NOWRAP, "+");
   uiInitDigits(&pageDigits, PAGE_WEIGHT, PAGE_SIZE);
   int fieldTextW = CONTENT_RIGHT - FIELD_PAD - pageCountReserve() - FIELD_TEXT_GAP - FIELD_TEXT_X;
   uiInitTextLabel(&fieldLabel, FIELD_WEIGHT, 0, 0, fieldTextW, FIELD_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   for (int i = 0; i < SORT_OPTIONS_MAX; i++)
      uiInitLabel(&sortLabels[i], OPTION_WEIGHT, 0, 0, AUTO, OPTION_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, NULL);
   uiInitLabel(&statusLabel, STATUS_WEIGHT, 0, 0, AUTO, STATUS_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   initIcon(&fieldIcon, ICON_SEARCH, FIELD_ICON_SIZE);
   search.noticeVisible = 0;
   search.fieldFocused  = 0;
   search.buttonFocused = 0;
   uiInitLabel(&subscribeLabels[0], UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, BUTTON_TEXT, activeTheme->onAccent, TEXT_NOWRAP, "Subscribe");
   uiInitLabel(&subscribeLabels[1], UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, BUTTON_TEXT, activeTheme->textPrimary, TEXT_NOWRAP, "Subscribed");
   initIcon(&subscribedIcon, ICON_OK, BUTTON_ICON);
   search.noticePanelW  = search.screenW * 2 / 3;
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, search.noticePanelW - 2 * NOTICE_PAD_X, NOTICE_TEXT, activeTheme->textPrimary, TEXT_WRAP, "");
   // the box's own line: the console's cross, where the old text spelled "Press X to close"
   uiInitHints(&noticeHints);
   uiAddHint(&noticeHints, GLYPH_CROSS, "Close");

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
   uiAddHint(&hints, GLYPH_DPAD_LEFT, "Menu");   // the rail opens here as on Home
   fitHintRow(&hints);

   initVideoGrid(&search.grid, uiGetFont(UI_CARD_TITLE_WEIGHT), CONTENT_X, GRID_TOP,
                 CONTENT_W, GRID_BOTTOM - GRID_TOP);
   setGridView(&search.grid, GRID_VIEW_LIST);   // results are rows (Bild 5), three to a page
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchLaterPredicate(&search.grid, tileInWatchLater);
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

   // The account's lists may arrive while a channel page is up (the first load after a sign-in): the Triangle hint
   // then says what Triangle really does. A change YouTube refused says so in the notice box.
   syncAccountLists();
   static unsigned listsSeen;
   if (getAccountListsRevision() != listsSeen) { listsSeen = getAccountListsRevision(); updateTriangleHint(); }
   if (!search.noticeVisible) {
      char failure[160];
      if (takeAccountFailure(failure, sizeof failure)) showNotice(failure);
   }

   if (search.noticeVisible) {   // modal: nothing underneath sees the pad until it is dismissed
      if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_CIRCLE)) search.noticeVisible = 0;
      return;
   }

   // The rail: LEFT opens it here as on Home (home.h), and while it is open the list only keeps loading and
   // decoding. X on Search in it means a new search, as START does here; O or RIGHT only close the rail, back to
   // the result or the field you were on.
   int rail = updateRailOverScreen(SIDE_SEARCH);
   if (rail == RAIL_GONE) return;   // Home is back on top: this screen is gone, touch nothing of it
   if (rail != RAIL_NOT_OPEN) {
      setGridInputSuppressed(&search.grid, 1);
      updateVideoGrid(&search.grid);
      if (!isSidebarFocused()) setGridInputSuppressed(&search.grid, search.fieldFocused || search.buttonFocused);
      if (rail == RAIL_PICKED_OWN) openSearchInput(search.query, onQueryEntered);
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

   // A channel page's Subscribe button has the focus: X toggles, UP goes on to the field, DOWN back to the videos.
   if (search.buttonFocused) {
      updateVideoGrid(&search.grid);
      if (isPadButtonPressed(PAD_BTN_DOWN)) { setButtonFocused(0); return; }
      if (isPadButtonPressed(PAD_BTN_UP))   { setFieldFocused(1); return; }
      if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_TRIANGLE)) toggleSubscription();
      return;
   }

   // The field has the focus: X opens the keyboard at once - on the query to edit it, or empty in a channel,
   // which has no query - and DOWN hands the focus back to the list (on a channel page, to its Subscribe button
   // first). The list keeps loading meanwhile.
   if (search.fieldFocused) {
      updateVideoGrid(&search.grid);
      if (isPadButtonPressed(PAD_BTN_DOWN)) { if (search.channelId[0]) setButtonFocused(1); else setFieldFocused(0); return; }
      if (isPadButtonPressed(PAD_BTN_CROSS))
         oskInputBegin("Search YouTube", search.channelId[0] ? "" : search.query, onFieldTyped);
      return;
   }

   // UP on the first result - or with no list to move in - goes up into the field, the way back to typing
   // without START. Only a press: a held UP that scrolls the list to its top stops there.
   if (isPadButtonPressed(PAD_BTN_UP) &&
       (gridStage(&search.grid) != GRID_READY || gridSelectedIndex(&search.grid) == 0)) {
      if (search.channelId[0]) setButtonFocused(1);   // a channel page: its Subscribe button first
      else setFieldFocused(1);
      updateVideoGrid(&search.grid);
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
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      if (isAccountSignedIn()) setAccountWatchLater(selected->videoId, !isInAccountWatchLater(selected->videoId));
      else toggleWatchLater(selected);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_R3)) { if (!selected->isLive) openDownloadChoice(selected); return; }
   if (isPadButtonPressed(PAD_BTN_L3)) { openPlaylistChooser(selected); return; }   // L3 came free with the grid layout   // live can't be downloaded
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { enterChannel(selected); return; }   // channel mode handled above
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatchedItem(selected);   // full entry, so History can list it
      stopVideoGrid(&search.grid);
      // hand over the whole list so autoplay can carry on with what follows
      playVideoFromList(gridResults(&search.grid), gridSelectedIndex(&search.grid));
   }
}

// The field as Bild 5 draws it: surface-2 with a 2-px inner border. The border is the hairline laid over the
// whole field and the field colour drawn again 2 px in, which leaves exactly the lightened edge of the CSS
// inset border.
static void fillField(int x, int y, int w, int h)
{
   uiFillRounded(x, y, w, h, FIELD_RADIUS, activeTheme->surface);
   uiFillRounded(x, y, w, h, FIELD_RADIUS, activeTheme->hairline);
   uiFillRounded(x + FIELD_BORDER, y + FIELD_BORDER, w - 2 * FIELD_BORDER, h - 2 * FIELD_BORDER,
                 FIELD_RADIUS - FIELD_BORDER, activeTheme->surface);
}

// An icon with its ink centred in a box. The glyph does not fill its em box, so centring the em box would
// leave the ink off centre by the glyph's own bearings.
static void drawIconInBox(Icon *icon, int boxX, int boxY, int boxW, int boxH, uint32_t color)
{
   if (!icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   drawIcon(icon, boxX + (boxW - icon->tex->w) / 2 - icon->offsetX, boxY + (boxH - icon->tex->h) / 2 - icon->offsetY, color);
}

// "Page 3 / 12+" right-aligned in the field's right end, its capitals centred in the field like the query's.
// Only once results are in; the "+" while the feed can still grow.
static void drawPageCount(void)
{
   char numbers[32];
   snprintf(numbers, sizeof numbers, " %d / %d", gridPageIndex(&search.grid) + 1, gridPageCount(&search.grid));
   int more    = gridHasMorePages(&search.grid);
   int digitsW = uiGetDigitsWidth(&pageDigits, numbers);
   int x = CONTENT_RIGHT - FIELD_PAD - pageWordLabel.tt.tex.w - digitsW - (more ? pagePlusLabel.tt.tex.w : 0);
   uiDrawLabelAt(&pageWordLabel, x, uiLabelYForCenter(&pageWordLabel, FIELD_Y, FIELD_H));
   x += pageWordLabel.tt.tex.w;
   uiDrawDigits(&pageDigits, x, uiTextYForCenter(pageDigits.font, pageDigits.size, FIELD_Y, FIELD_H), numbers,
                activeTheme->textTertiary);
   x += digitsW;
   if (more) uiDrawLabelAt(&pagePlusLabel, x, uiLabelYForCenter(&pagePlusLabel, FIELD_Y, FIELD_H));
}

static int sortSegmentWidth(void)
{
   int width = 2 * SEGMENT_PAD + (search.sortCount - 1) * SEGMENT_GAP;
   for (int i = 0; i < search.sortCount; i++) width += sortLabels[i].tt.tex.w + 2 * OPTION_PAD_X;
   return width;
}

// the current mode's sorts in a row, right-aligned under the field, the one in force on the accent
static void drawSortSegment(void)
{
   int current = search.channelId[0] ? search.channelSort : search.sort;
   int width = sortSegmentWidth();
   int x = CONTENT_RIGHT - width, optionY = SEGMENT_Y + SEGMENT_PAD;
   uiFillRounded(x, SEGMENT_Y, width, SEGMENT_H, SEGMENT_RADIUS, activeTheme->surface1);
   x += SEGMENT_PAD;
   for (int i = 0; i < search.sortCount; i++) {
      int optionW = sortLabels[i].tt.tex.w + 2 * OPTION_PAD_X;
      if (i == current) uiFillRounded(x, optionY, optionW, OPTION_H, OPTION_RADIUS, activeTheme->accent);
      uiDrawLabelAt(&sortLabels[i], x + OPTION_PAD_X, uiLabelYForCenter(&sortLabels[i], optionY, OPTION_H));
      x += optionW + SEGMENT_GAP;
   }
}

static int subscribeButtonWidth(int following)
{
   int width = 2 * BUTTON_PAD_X + subscribeLabels[following].tt.tex.w;
   if (following && subscribedIcon.tex) width += BUTTON_ICON + BUTTON_ICON_GAP;
   return width;
}

// a channel page's Subscribe button, left of the sort segment on its row; the state re-read on every draw
static void drawSubscribeButton(void)
{
   int following = followsChannel(search.channelId) ? 1 : 0;
   int width = subscribeButtonWidth(following), x = CONTENT_RIGHT - sortSegmentWidth() - BUTTON_GAP - width;
   if (search.buttonFocused) uiDrawFocusGlow(x, SEGMENT_Y, width, SEGMENT_H, activeTheme->focusGlow);
   uiFillPill(x, SEGMENT_Y, width, SEGMENT_H, following ? activeTheme->surface3 : activeTheme->accent);
   int textX = x + BUTTON_PAD_X;
   if (following && subscribedIcon.tex) {
      drawIconInBox(&subscribedIcon, textX, SEGMENT_Y, BUTTON_ICON, SEGMENT_H, activeTheme->textPrimary);
      textX += BUTTON_ICON + BUTTON_ICON_GAP;
   }
   const Label *word = &subscribeLabels[following];
   uiDrawLabelAt(word, textX, uiLabelYForCenter(word, SEGMENT_Y, SEGMENT_H));
   if (search.buttonFocused) uiDrawFocusRing(x, SEGMENT_Y, width, SEGMENT_H, UI_RING_ROW, activeTheme->focusBorder);
}

// Bild 5's header: the field with what the results are for and the page count, the sort segment under it
// and, at the other end of the segment's row, a running download's chip. Drawn after the list, as the
// focused row's glow reaches 81 px up into it.
static void drawHeader(void)
{
   // with the focus on the field it looks as on the search field screen: the glow first, under the field, the
   // magnifier orange, the ring over it all
   if (search.fieldFocused) uiDrawFocusGlow(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H, activeTheme->focusGlow);
   fillField(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H);
   drawIconInBox(&fieldIcon, CONTENT_X + FIELD_PAD, FIELD_Y, FIELD_ICON_BOX, FIELD_H,
                 search.fieldFocused ? activeTheme->accent : activeTheme->textSecondary);
   uiDrawLabelAt(&fieldLabel, FIELD_TEXT_X, uiLabelYForCenter(&fieldLabel, FIELD_Y, FIELD_H));
   if (gridStage(&search.grid) == GRID_READY) drawPageCount();
   if (search.fieldFocused) uiDrawFocusRing(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H, UI_RING_ROW, activeTheme->focusBorder);
   drawSortSegment();
   if (search.channelId[0]) drawSubscribeButton();
   drawDownloadOverlayAt(CONTENT_X, DOWNLOAD_CHIP_Y, UI_ALIGN_LEFT);   // nothing while no download runs
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
   uiDrawHints(&noticeHints, x + NOTICE_PAD_X, y + textTop + noticeLabel.tt.tex.h + NOTICE_GAP, UI_ALIGN_LEFT);
}

static void drawSearch(void)
{
   fillGfxRectangle(0, 0, search.screenW, search.screenH, activeTheme->appBg);

   // The list goes down first and the header over it: the focused row's glow reaches 81 px out from its
   // plate, up into the header, and text drawn before it would come out tinted orange.
   if (gridStage(&search.grid) == GRID_READY) {
      drawVideoGrid(&search.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
      uiDrawLabelAt(&statusLabel, CONTENT_X, uiLabelYForBaseline(&statusLabel, STATUS_BASELINE));
   }
   drawHeader();

   if (!isSidebarFocused()) drawSidebarPassive(search.screenH, SIDE_SEARCH);
   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   // The rail, open, goes over everything - the hints included: with eleven of them the row reaches in under the
   // open panel, and drawn over it they would run into its names. Its scrim dims them with the rest.
   if (isSidebarFocused()) drawSidebar(search.screenH);

   if (search.noticeVisible) drawNotice();
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   uiTermHints(&hints);
   uiTermHints(&noticeHints);
   freeLabel(&fieldLabel);
   freeLabel(&statusLabel);
   freeLabel(&pageWordLabel);
   freeLabel(&pagePlusLabel);
   for (int i = 0; i < SORT_OPTIONS_MAX; i++) freeLabel(&sortLabels[i]);
   freeLabel(&noticeLabel);
   freeLabel(&subscribeLabels[0]);
   freeLabel(&subscribeLabels[1]);
   freeIcon(&subscribedIcon);
   uiFreeDigits(&pageDigits);
   freeIcon(&fieldIcon);   // the texture belongs to the icon font's shared cache; this only lets go of it
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
   // (an empty field and no channel) and whose Triangle hint lies about what the button does.
   if (!item->channelId[0]) return;
   pendingChannel = *item;
   pendingIsChannel = 1;
   pushScreen(&searchScreen);
}

Screen searchScreen = { initSearch, resumeSearch, updateSearch, drawSearch, NULL, termSearch, SCREEN_TERMINATED };

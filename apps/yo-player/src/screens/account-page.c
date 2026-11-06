// account page - implementation (see account-page.h). Two pages in the shape of the region picker: a list of rows,
// the focused one on a plate with the focus ring and its glow, under a title with a note. The menu has a row per
// list and Sign out at its foot; the channel list is every followed channel, A to Z, in a window that follows the
// selection - its rows' labels are rendered only for the rows in view, as an account may follow hundreds.

#include "screens/account-page.h"
#include "screens/home.h"         // showHomeSection, and the rail over these pages (updateRailOverScreen)
#include "screens/search.h"       // openChannelFor: X on a channel
#include "account.h"
#include "sidebar.h"
#include "ui-clock.h"
#include "ui-hints.h"             // UiHints; brings ui-draw.h and ui-fonts.h
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "anim.h"                 // interpolateColor: the focused plate's colour
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include "string-utilities.h"     // strCopy
#include "thread.h"               // the channel list's worker
#include "dbg.h"
#include <sys/sys_time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CONTENT_X      176
#define HEADER_Y       67
#define HEADER_H       45
#define HEADER_GAP     40
#define CLOCK_SIZE     28
#define CLOCK_WEIGHT   UI_WEIGHT_BOLD
#define POS_SIZE       24
#define POS_WEIGHT     UI_WEIGHT_BOLD
#define NOTE_DY        43       // the title's baseline -> the note's, where Settings has its version line
#define NOTE_LIST_GAP  42       // the note's last baseline -> the first plate
#define ROW_H          70
#define PLATE_H        (ROW_H - 8)
#define LIST_W         1152
#define ROW_TEXT       26
#define ROW_WEIGHT     UI_WEIGHT_BOLD
#define ROW_PAD        22
#define ICON_SIZE      28
#define ICON_GAP       18
#define LIST_CLEAR     30       // the last plate ends at least this far above the hint row
#define SLOTS          20       // channel rows rendered at once: more than a window holds
#define NOTICE_PAD_X   44
#define NOTICE_PAD_TOP 40
#define NOTICE_GAP     24
#define NOTICE_PAD_BOTTOM 30
#define NOTICE_RADIUS  26
#define TEXT_TINTABLE  0xFFFFFFFFu

// ---- what both pages share ----

static uint32_t plateFill(void)
{
   return interpolateColor(activeTheme->appBg, uiWithAlpha(activeTheme->hintFill, 0xFF),
                           (float)(activeTheme->hintFill >> 24) / 255.0f);
}

static int lastBaselineOf(const Label *l, int firstBaseline)
{
   UiTextMetrics m = uiGetTextMetrics(l->font, l->size);
   int extra = l->tt.tex.h - m.texH;
   return firstBaseline + (extra > 0 ? extra : 0);
}

static int rowBaseline(int plateY)
{
   Font *font = uiGetFont(ROW_WEIGHT);
   return uiTextYForCenter(font, ROW_TEXT, plateY, PLATE_H) + uiGetTextMetrics(font, ROW_TEXT).baseline;
}

static void drawFocusPlate(int y, uint32_t fill)
{
   uiDrawFocusGlow(CONTENT_X, y, LIST_W, PLATE_H, activeTheme->focusGlow);
   uiFillRounded(CONTENT_X, y, LIST_W, PLATE_H, UI_RADIUS_ROW, fill);
   uiDrawFocusRing(CONTENT_X, y, LIST_W, PLATE_H, UI_RING_CARD, activeTheme->focusBorder);
}

static void drawRowIcon(Icon *icon, int plateY, uint32_t tint)
{
   if (!icon->tex) return;
   drawIcon(icon, CONTENT_X + ROW_PAD, plateY + (PLATE_H - icon->tex->h) / 2 - icon->offsetY, tint);
}

static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

typedef struct { int titleY, noteBaseline, clockBaseline, listTop; } Header;

static void layOutHeader(Header *h, const Label *title, const Label *note)
{
   h->titleY       = uiLabelYForCenter(title, HEADER_Y, HEADER_H);
   h->noteBaseline = h->titleY + uiGetTextMetrics(title->font, title->size).baseline + NOTE_DY;
   h->listTop      = lastBaselineOf(note, h->noteBaseline) + NOTE_LIST_GAP;
   Font *clockFont  = uiGetFont(CLOCK_WEIGHT);
   h->clockBaseline = uiTextYForCenter(clockFont, CLOCK_SIZE, HEADER_Y, HEADER_H) + uiGetTextMetrics(clockFont, CLOCK_SIZE).baseline;
}

static void drawHeader(const Header *h, const Label *title, const Label *note)
{
   uiDrawLabelAt(title, CONTENT_X, h->titleY);
   uiDrawLabelAt(note, CONTENT_X, uiLabelYForBaseline(note, h->noteBaseline));
   uiDrawClock(UI_SAFE_RIGHT, h->clockBaseline);
}

// ---- the menu ----

typedef enum { ROW_FOR_YOU, ROW_SUBS, ROW_CHANNELS, ROW_LATER, ROW_HISTORY, ROW_LIKED, ROW_PLAYLISTS, ROW_SIGN_OUT,
               MENU_ROWS } MenuRow;

static const struct { IconId icon; const char *name; } MENU[MENU_ROWS] = {
   { ICON_HAND_POINTER_O, "For you" },
   { ICON_STAR,           "Subscriptions" },
   { ICON_SORT,           "All channels" },
   { ICON_CLOCK,          "Watch Later" },
   { ICON_HISTORY,        "History" },
   { ICON_OK,             "Liked videos" },
   { ICON_DOCS,           "Playlists" },
   { ICON_LOCK_OPEN,      "Sign out" },
};

static Label   menuTitle, menuNote;
static Label   menuNames[MENU_ROWS], menuDetails[MENU_ROWS];
static Icon    menuIcons[MENU_ROWS];
static UiHints menuHints;
static ButtonRepeat menuRepeat;
static struct {
   int screenW, screenH, selected;
   Header header;
   uint32_t plate;
   unsigned listsSeen;
} menu;

static void openChannelList(void);

// The words at a row's right end: what the account holds there, as far as the lists loaded know it.
static void setMenuDetails(void)
{
   char text[64];
   int channels = getAccountFollowingCount(), later = getAccountWatchLaterCount();
   setLabelText(&menuDetails[ROW_FOR_YOU], "Recommended for you");
   setLabelText(&menuDetails[ROW_SUBS], "Newest uploads first");
   if (channels > 0) snprintf(text, sizeof text, "%d channels, A to Z", channels); else snprintf(text, sizeof text, "A to Z");
   setLabelText(&menuDetails[ROW_CHANNELS], text);
   if (later >= 0) snprintf(text, sizeof text, later == 1 ? "1 video" : "%d videos", later); else text[0] = 0;
   setLabelText(&menuDetails[ROW_LATER], text);
   setLabelText(&menuDetails[ROW_HISTORY], "What you watched on YouTube");
   text[0] = 0;
   AccountPlaylistInfo list;
   for (int i = 0; i < getAccountPlaylistCount(); i++)
      if (getAccountPlaylistAt(i, &list) && strcmp(list.id, "LL") == 0) { strCopy(text, sizeof text, list.count); break; }
   setLabelText(&menuDetails[ROW_LIKED], text);
   int playlists = getAccountPlaylistCount();
   if (playlists > 0) snprintf(text, sizeof text, "On YouTube and on this PS3"); else snprintf(text, sizeof text, "On this PS3");
   setLabelText(&menuDetails[ROW_PLAYLISTS], text);
   setLabelText(&menuDetails[ROW_SIGN_OUT], "Withdraws this console's sign-in at Google");
   menu.listsSeen = getAccountListsRevision();
}

static void initAccountPage(void)
{
   menu.screenW = getGfxScreenWidth();
   menu.screenH = getGfxScreenHeight();
   menu.selected = 0;
   menu.plate = plateFill();
   uiInitLabel(&menuTitle, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Account");
   uiInitLabel(&menuNote, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE,
               activeTheme->textSecondary, TEXT_WRAP,
               "Signed in to YouTube. These lists come from your account - so do Subscriptions, Watch Later, "
               "History and Playlists in the menu.");
   int nameW = LIST_W / 2;
   for (int i = 0; i < MENU_ROWS; i++) {
      uiInitLabel(&menuNames[i], ROW_WEIGHT, 0, 0, nameW, ROW_TEXT, TEXT_TINTABLE, TEXT_NOWRAP_ELLIPSIS, MENU[i].name);
      uiInitTextLabel(&menuDetails[i], UI_META_WEIGHT, 0, 0, AUTO, UI_META_SIZE, activeTheme->textSecondary,
                      TEXT_NOWRAP, "");
      initIcon(&menuIcons[i], MENU[i].icon, ICON_SIZE);
   }
   setMenuDetails();
   uiInitHints(&menuHints);
   uiAddHint(&menuHints, GLYPH_CROSS, "Open");
   uiAddHint(&menuHints, GLYPH_CIRCLE, "Back");
   uiAddHint(&menuHints, GLYPH_DPAD_LEFT, "Menu");
   layOutHeader(&menu.header, &menuTitle, &menuNote);
   uint64_t now = sys_time_get_system_time();
   menuRepeat.timer = now; menuRepeat.repeats = 0;
   uiLogVram("account page");
}

static void resumeAccountPage(void)
{
   menuRepeat.timer = sys_time_get_system_time();
   menuRepeat.repeats = 0;
   if (!isAccountSignedIn()) popScreen();   // signed out meanwhile (the sign-in was withdrawn): nothing to show
}

static void actOnMenuRow(int row)
{
   switch (row) {
   case ROW_FOR_YOU:   showHomeSection(HOME_SECTION_FOR_YOU); break;
   case ROW_SUBS:      showHomeSection(HOME_SECTION_SUBSCRIPTIONS); break;
   case ROW_CHANNELS:  openChannelList(); break;
   case ROW_LATER:     showHomeSection(HOME_SECTION_WATCH_LATER); break;
   case ROW_HISTORY:   showHomeSection(HOME_SECTION_HISTORY); break;
   case ROW_LIKED:     showHomeSection(HOME_SECTION_LIKED); break;
   case ROW_PLAYLISTS: showHomeSection(HOME_SECTION_PLAYLISTS); break;
   case ROW_SIGN_OUT:  signOutAccount(); popScreen(); break;   // Home reloads its pages as signed out
   }
}

static void updateAccountPage(void)
{
   syncAccountLists();
   if (getAccountListsRevision() != menu.listsSeen) setMenuDetails();

   int rail = updateRailOverScreen(SIDE_ACCOUNT);
   if (rail == RAIL_GONE) return;
   if (rail != RAIL_NOT_OPEN) {
      if (!isSidebarFocused()) { menuRepeat.timer = sys_time_get_system_time(); menuRepeat.repeats = 0; }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }
   if      (isRepeatDue(&menuRepeat, getPadButtonState(PAD_BTN_DOWN))) menu.selected++;
   else if (isRepeatDue(&menuRepeat, getPadButtonState(PAD_BTN_UP)))   menu.selected--;
   if (menu.selected < 0) menu.selected = MENU_ROWS - 1;
   if (menu.selected >= MENU_ROWS) menu.selected = 0;
   if (isPadButtonPressed(PAD_BTN_CROSS)) actOnMenuRow(menu.selected);
}

static void drawAccountPage(void)
{
   fillGfxRectangle(0, 0, menu.screenW, menu.screenH, activeTheme->appBg);
   if (!isSidebarFocused()) drawSidebarPassive(menu.screenH, SIDE_ACCOUNT);
   drawFocusPlate(menu.header.listTop + menu.selected * ROW_H, menu.plate);
   for (int row = 0; row < MENU_ROWS; row++) {
      int y = menu.header.listTop + row * ROW_H, picked = row == menu.selected, signOut = row == ROW_SIGN_OUT;
      drawRowIcon(&menuIcons[row], y, picked ? activeTheme->accent : signOut ? activeTheme->accentText : activeTheme->textSecondary);
      int baseline = rowBaseline(y);
      drawTinted(&menuNames[row], CONTENT_X + ROW_PAD + ICON_SIZE + ICON_GAP, uiLabelYForBaseline(&menuNames[row], baseline),
                 signOut ? activeTheme->accentText : activeTheme->textPrimary);
      const Label *detail = &menuDetails[row];
      uiDrawLabelAt(detail, CONTENT_X + LIST_W - ROW_PAD - detail->tt.tex.w, uiLabelYForBaseline(detail, baseline));
   }
   drawHeader(&menu.header, &menuTitle, &menuNote);   // after the plate: its glow reaches up under the note
   uiDrawHints(&menuHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   if (isSidebarFocused()) drawSidebar(menu.screenH);
}

static void termAccountPage(void)
{
   uiTermHints(&menuHints);
   freeLabel(&menuTitle);
   freeLabel(&menuNote);
   for (int i = 0; i < MENU_ROWS; i++) { freeLabel(&menuNames[i]); freeLabel(&menuDetails[i]); freeIcon(&menuIcons[i]); }
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen accountPageScreen = { initAccountPage, resumeAccountPage, updateAccountPage, drawAccountPage, NULL,
                                    termAccountPage, SCREEN_TERMINATED };

void openAccountPage(void) { pushScreen(&accountPageScreen); }

// ---- all channels, A to Z ----

static Label   chanTitle, chanNote, chanStatus, noticeLabel;
static Label   chanNames[SLOTS], chanDetails[SLOTS];
static int     slotRow[SLOTS];                 // which row each slot's labels hold, -1 none
static Icon    chanIcon;
static UiDigits posDigits;
static UiHints chanHints, confirmHints;
static ButtonRepeat chanRepeat, chanPageRepeat;

static SearchResults *channels;                 // the list on screen (owned by the UI thread once loaded)
static SearchResults *loadedChannels;           // the worker's result, handed over under listLock
static sys_lwmutex_t listLock;
static int           listLockReady;
static volatile unsigned listAttempt;           // a worker whose attempt is no longer this one drops its result
static volatile int  listState;                 // 0 loading, 1 ready (loadedChannels set), -1 failed

static struct {
   int screenW, screenH, selected, top, visible, count;
   int noticeVisible;
   Header header;
   uint32_t plate;
   char pendingId[32], pendingName[160];
} chan;

static int nameLess(const SearchResult *a, const SearchResult *b)
{
   for (const unsigned char *x = (const unsigned char *)a->title, *y = (const unsigned char *)b->title; ; x++, y++) {
      unsigned char cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
      if (cx != cy || !cx) return cx < cy;
   }
}

static int compareNames(const void *a, const void *b)
{
   const SearchResult *x = (const SearchResult *)a, *y = (const SearchResult *)b;
   return nameLess(x, y) ? -1 : nameLess(y, x) ? 1 : 0;
}

static void channelWorker(uint64_t arg)
{
   unsigned mine = (unsigned)arg;
   SearchResults *all = (SearchResults *)malloc(sizeof *all), *page = (SearchResults *)malloc(sizeof *page);
   char *token = (char *)malloc(MAX_CONTINUATION);
   int ok = all && page && token;
   if (ok) { memset(all, 0, sizeof *all); token[0] = 0; }
   for (int pageNumber = 0; ok && pageNumber < 10; pageNumber++) {
      if (mine != listAttempt) { ok = 0; break; }
      if (getAccountFeed(ACCOUNT_FEED_CHANNELS, token[0] ? token : NULL, page) != 0) { ok = pageNumber > 0; break; }
      for (int i = 0; i < page->count && all->count < MAX_SEARCH_RESULTS; i++) all->items[all->count++] = page->items[i];
      if (!page->continuation[0] || all->count >= MAX_SEARCH_RESULTS) break;
      strCopy(token, MAX_CONTINUATION, page->continuation);
   }
   if (ok) qsort(all->items, all->count, sizeof all->items[0], compareNames);
   free(page);
   free(token);
   lock(&listLock);
   if (ok && mine == listAttempt) { loadedChannels = all; all = NULL; listState = 1; }
   else if (mine == listAttempt) listState = -1;
   unlock(&listLock);
   free(all);
   exitThread();
}

static void followSelection(void)
{
   if (chan.selected < chan.top) chan.top = chan.selected;
   if (chan.selected >= chan.top + chan.visible) chan.top = chan.selected - chan.visible + 1;
   int maxTop = chan.count - chan.visible;
   if (chan.top > maxTop) chan.top = maxTop;
   if (chan.top < 0) chan.top = 0;
}

// a row's labels, rendered into slot row % SLOTS when that slot holds another row
static int slotFor(int row)
{
   int slot = row % SLOTS;
   if (slotRow[slot] != row) {
      const SearchResult *item = &channels->items[row];
      uiSetTextLabel(&chanNames[slot], ROW_WEIGHT, item->title[0] ? item->title : item->channelId);
      uiSetTextLabel(&chanDetails[slot], UI_META_WEIGHT, item->views);
      slotRow[slot] = row;
   }
   return slot;
}

static void setChannelNote(void)
{
   char text[160];
   if (listState == 0) snprintf(text, sizeof text, "Every channel your account follows, A to Z.");
   else snprintf(text, sizeof text, "%d channels your account follows, A to Z. X opens a channel, Triangle unsubscribes.",
                 chan.count);
   uiSetTextLabel(&chanNote, UI_BODY_WEIGHT, text);
}

static void initChannelList(void)
{
   chan.screenW = getGfxScreenWidth();
   chan.screenH = getGfxScreenHeight();
   chan.selected = chan.top = chan.count = 0;
   chan.noticeVisible = 0;
   chan.plate = plateFill();
   if (!listLockReady && createLock(&listLock) == 0) listLockReady = 1;
   for (int i = 0; i < SLOTS; i++) slotRow[i] = -1;

   uiInitLabel(&chanTitle, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "All channels");
   uiInitTextLabel(&chanNote, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE,
                   activeTheme->textSecondary, TEXT_WRAP, "");
   uiInitLabel(&chanStatus, UI_WEIGHT_BOLD, 0, 0, AUTO, 24, activeTheme->textSecondary, TEXT_NOWRAP, "Loading...");
   int nameW = LIST_W - 2 * ROW_PAD - ICON_SIZE - ICON_GAP - 260;
   for (int i = 0; i < SLOTS; i++) {
      uiInitTextLabel(&chanNames[i], ROW_WEIGHT, 0, 0, nameW, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&chanDetails[i], UI_META_WEIGHT, 0, 0, AUTO, UI_META_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, chan.screenW * 2 / 3 - 2 * NOTICE_PAD_X, 24,
                   activeTheme->textPrimary, TEXT_WRAP, "");
   initIcon(&chanIcon, ICON_STAR, ICON_SIZE);
   uiInitDigits(&posDigits, POS_WEIGHT, POS_SIZE);
   uiInitHints(&chanHints);
   uiAddHint(&chanHints, GLYPH_CROSS, "Open channel");
   uiAddHint(&chanHints, GLYPH_TRIANGLE, "Unsubscribe");
   uiAddHint(&chanHints, GLYPH_CIRCLE, "Back");
   uiAddHint(&chanHints, GLYPH_L2, "");
   uiAddHint(&chanHints, GLYPH_R2, "Page");
   uiInitHints(&confirmHints);
   uiAddHint(&confirmHints, GLYPH_CROSS, "Unsubscribe");
   uiAddHint(&confirmHints, GLYPH_CIRCLE, "Cancel");

   lock(&listLock);
   unsigned mine = ++listAttempt;
   listState = 0;
   unlock(&listLock);
   setChannelNote();
   layOutHeader(&chan.header, &chanTitle, &chanNote);
   chan.visible = (UI_HINTS_BOTTOM_Y - LIST_CLEAR - chan.header.listTop - PLATE_H) / ROW_H + 1;
   if (chan.visible < 1) chan.visible = 1;
   sys_ppu_thread_t tid;
   if (spawnThread(&tid, channelWorker, (uint64_t)mine, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB,
                   "account-channels") != 0) listState = -1;
   uint64_t now = sys_time_get_system_time();
   chanRepeat.timer = chanPageRepeat.timer = now;
   chanRepeat.repeats = chanPageRepeat.repeats = 0;
}

static void resumeChannelList(void)
{
   uint64_t now = sys_time_get_system_time();
   chanRepeat.timer = chanPageRepeat.timer = now;
   chanRepeat.repeats = chanPageRepeat.repeats = 0;
}

static void askUnsubscribe(void)
{
   const SearchResult *item = &channels->items[chan.selected];
   strCopy(chan.pendingId, sizeof chan.pendingId, item->channelId);
   strCopy(chan.pendingName, sizeof chan.pendingName, item->title);
   char message[224];
   snprintf(message, sizeof message, "Unsubscribe from %s?", item->title[0] ? item->title : "this channel");
   uiSetTextLabel(&noticeLabel, UI_BODY_WEIGHT, message);
   chan.noticeVisible = 1;
}

// the account unsubscribes (YouTube told on a worker); the row goes at once
static void unsubscribeConfirmed(void)
{
   setAccountFollowing(chan.pendingId, 0);
   int out = 0;
   for (int i = 0; i < chan.count; i++)
      if (strcmp(channels->items[i].channelId, chan.pendingId) != 0) channels->items[out++] = channels->items[i];
   channels->count = chan.count = out;
   for (int i = 0; i < SLOTS; i++) slotRow[i] = -1;   // rows moved up: every slot renders afresh
   if (chan.selected >= chan.count) chan.selected = chan.count - 1;
   if (chan.selected < 0) chan.selected = 0;
   followSelection();
   setChannelNote();
}

static void updateChannelList(void)
{
   if (listState == 1 && !channels) {   // the worker's list arrives
      lock(&listLock);
      channels = loadedChannels;
      loadedChannels = NULL;
      unlock(&listLock);
      chan.count = channels ? channels->count : 0;
      setChannelNote();
      if (!chan.count) setLabelText(&chanStatus, "Your account follows no channels.");
   }
   if (listState == -1 && !channels) setLabelText(&chanStatus, "The channel list could not be loaded. O goes back.");

   if (chan.noticeVisible) {
      if (isPadButtonPressed(PAD_BTN_CROSS)) { chan.noticeVisible = 0; unsubscribeConfirmed(); return; }
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) chan.noticeVisible = 0;
      return;
   }
   int rail = updateRailOverScreen(SIDE_ACCOUNT);
   if (rail == RAIL_GONE) return;
   if (rail != RAIL_NOT_OPEN) { if (!isSidebarFocused()) resumeChannelList(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }
   if (!channels || chan.count == 0) return;

   if      (isRepeatDue(&chanRepeat, getPadButtonState(PAD_BTN_DOWN))) chan.selected++;
   else if (isRepeatDue(&chanRepeat, getPadButtonState(PAD_BTN_UP)))   chan.selected--;
   if      (isRepeatDue(&chanPageRepeat, getPadButtonState(PAD_BTN_R2))) chan.selected += chan.visible;
   else if (isRepeatDue(&chanPageRepeat, getPadButtonState(PAD_BTN_L2))) chan.selected -= chan.visible;
   if (chan.selected < 0) chan.selected = 0;
   if (chan.selected >= chan.count) chan.selected = chan.count - 1;
   followSelection();

   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { askUnsubscribe(); return; }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      SearchResult channel = channels->items[chan.selected];
      strCopy(channel.author, sizeof channel.author, channel.title);   // openChannelFor reads the name from author
      openChannelFor(&channel);
   }
}

static void drawNotice(void)
{
   UiTextMetrics metrics = uiGetTextMetrics(noticeLabel.font, noticeLabel.size);
   int textTop = NOTICE_PAD_TOP - metrics.capTop;
   if (textTop < 0) textTop = 0;
   int panelW = chan.screenW * 2 / 3;
   int panelH = textTop + noticeLabel.tt.tex.h + NOTICE_GAP + UI_HINT_GLYPH_H + NOTICE_PAD_BOTTOM;
   int x = (chan.screenW - panelW) / 2, y = (chan.screenH - panelH) / 2;
   fillGfxRectangle(0, 0, chan.screenW, chan.screenH, activeTheme->scrim);
   uiFillRounded(x, y, panelW, panelH, NOTICE_RADIUS, activeTheme->surface);
   uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);
   uiDrawLabelAt(&noticeLabel, x + NOTICE_PAD_X, y + textTop);
   uiDrawHints(&confirmHints, x + NOTICE_PAD_X, y + textTop + noticeLabel.tt.tex.h + NOTICE_GAP, UI_ALIGN_LEFT);
}

static void drawChannelList(void)
{
   fillGfxRectangle(0, 0, chan.screenW, chan.screenH, activeTheme->appBg);
   if (!isSidebarFocused()) drawSidebarPassive(chan.screenH, SIDE_ACCOUNT);

   if (channels && chan.count > 0) {
      int end = chan.top + chan.visible;
      if (end > chan.count) end = chan.count;
      drawFocusPlate(chan.header.listTop + (chan.selected - chan.top) * ROW_H, chan.plate);
      for (int row = chan.top; row < end; row++) {
         int y = chan.header.listTop + (row - chan.top) * ROW_H, slot = slotFor(row);
         drawRowIcon(&chanIcon, y, row == chan.selected ? activeTheme->accent : activeTheme->textSecondary);
         int baseline = rowBaseline(y);
         uiDrawLabelAt(&chanNames[slot], CONTENT_X + ROW_PAD + ICON_SIZE + ICON_GAP, uiLabelYForBaseline(&chanNames[slot], baseline));
         uiDrawLabelAt(&chanDetails[slot], CONTENT_X + LIST_W - ROW_PAD - chanDetails[slot].tt.tex.w,
                       uiLabelYForBaseline(&chanDetails[slot], baseline));
      }
      // "7 / 120" left of the clock, as the region picker shows where in a long list you are
      char position[24];
      snprintf(position, sizeof position, "%d / %d", chan.selected + 1, chan.count);
      int right = UI_SAFE_RIGHT - uiGetClockWidth() - HEADER_GAP;
      uiDrawDigits(&posDigits, right - uiGetDigitsWidth(&posDigits, position),
                   uiTextYForCenter(posDigits.font, posDigits.size, HEADER_Y, HEADER_H), position, activeTheme->textSecondary);
   } else {
      uiDrawLabelAt(&chanStatus, CONTENT_X, uiLabelYForBaseline(&chanStatus, rowBaseline(chan.header.listTop)));
   }
   drawHeader(&chan.header, &chanTitle, &chanNote);
   uiDrawHints(&chanHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   if (isSidebarFocused()) drawSidebar(chan.screenH);
   if (chan.noticeVisible) drawNotice();
}

static void termChannelList(void)
{
   lock(&listLock);
   listAttempt++;   // a worker still asking drops its answer
   free(loadedChannels);
   loadedChannels = NULL;
   unlock(&listLock);
   free(channels);
   channels = NULL;
   uiTermHints(&chanHints);
   uiTermHints(&confirmHints);
   uiFreeDigits(&posDigits);
   freeIcon(&chanIcon);
   freeLabel(&chanTitle);
   freeLabel(&chanNote);
   freeLabel(&chanStatus);
   freeLabel(&noticeLabel);
   for (int i = 0; i < SLOTS; i++) { freeLabel(&chanNames[i]); freeLabel(&chanDetails[i]); }
}

static Screen channelListScreen = { initChannelList, resumeChannelList, updateChannelList, drawChannelList, NULL,
                                    termChannelList, SCREEN_TERMINATED };

static void openChannelList(void) { pushScreen(&channelListScreen); }

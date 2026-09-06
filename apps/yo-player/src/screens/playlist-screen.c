// playlist screen - implementation (see playlist-screen.h). Laid out like search-input.c: a plain list of
// rows with a hint bar, which is the shape this app already uses for "pick one of these".

#include "screens/playlist-screen.h"
#include "storage.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define LIST_TOP      110
#define ROW_H         56
#define ROW_TEXT      25
#define ROW_PAD       18
#define COUNT_GAP     30
#define ICON_SIZE     26
#define LIST_WIDTH    900
#define HINTS_BOTTOM  42
#define HINT_GLYPH_H  25
#define HINT_TEXT     21
#define EMPTY_TEXT    23
#define NOTICE_PAD    28
#define NOTICE_TEXT   26
#define NOTICE_HINT   20

typedef enum { MODE_BROWSE, MODE_CHOOSE } PlaylistMode;

// In CHOOSE mode row 0 is "New playlist..." and the lists follow; in BROWSE mode the lists start at row 0
// and START makes a new one. Keeping the extra row out of BROWSE means the common case - open a list - has
// nothing in front of it.
#define CHOOSE_NEW_ROW 0

static Font        font;
static Label       titleLabel, emptyLabel;
static Label       rowLabels[MAX_PLAYLISTS], countLabels[MAX_PLAYLISTS], newLabel;
static Label       noticeLabel, noticeHintLabel;
static Icon        rowIcon, newIcon;
static ButtonHints hints;
static ButtonRepeat navigateRepeat;

static PlaylistMode  mode;
static SearchResult  pendingItem;                 // MODE_CHOOSE: what is being filed
static void        (*openCallback)(int playlist);
static PlaylistMode  pendingMode;
static SearchResult  pendingItemIn;
static void        (*pendingCallback)(int playlist);

static struct {
   int screenW, screenH;
   int selected;
   int noticeVisible;
   int confirmDelete;      // the notice is asking whether to delete `deleteTarget`
   int deleteTarget;
   int renderedCount;      // playlists the row labels were built for
} state;

static int rowCount(void)
{
   return getPlaylistCount() + (mode == MODE_CHOOSE ? 1 : 0);
}

static int playlistForRow(int row)
{
   return mode == MODE_CHOOSE ? row - 1 : row;
}

static void showNotice(const char *text, int isConfirm)
{
   setLabelText(&noticeLabel, text);
   setLabelText(&noticeHintLabel, isConfirm ? "X Delete     O Cancel" : "Press X to close");
   state.confirmDelete = isConfirm;
   state.noticeVisible = 1;
}

// rebuild the row labels; called on entry and after anything changes the set
static void refreshRows(void)
{
   int count = getPlaylistCount();
   for (int i = 0; i < count; i++) {
      setLabelText(&rowLabels[i], getPlaylistName(i));
      char counted[32];
      int items = getPlaylistItemCount(i);
      snprintf(counted, sizeof counted, items == 1 ? "%d video" : "%d videos", items);
      setLabelText(&countLabels[i], counted);
   }
   state.renderedCount = count;
   if (state.selected >= rowCount()) state.selected = rowCount() > 0 ? rowCount() - 1 : 0;
}

static void onNameTyped(const char *text)
{
   if (!text || !text[0]) return;
   int created = createPlaylist(text);
   if (created < 0) { showNotice("That is as many playlists as this app keeps.", 0); return; }

   if (mode == MODE_CHOOSE) {
      addToPlaylist(created, &pendingItem);
      popScreen();
      return;
   }
   refreshRows();
   state.selected = created;
}

static void activateRow(void)
{
   if (mode == MODE_CHOOSE && state.selected == CHOOSE_NEW_ROW) {
      oskInputBegin("Name the playlist", "", onNameTyped);
      return;
   }

   int playlist = playlistForRow(state.selected);
   if (playlist < 0 || playlist >= getPlaylistCount()) return;

   if (mode == MODE_CHOOSE) {
      int rc = addToPlaylist(playlist, &pendingItem);
      if (rc == PLAYLIST_FULL) { showNotice("That playlist is full.", 0); return; }
      popScreen();   // filed, silently: the list it went into is the confirmation
      return;
   }

   void (*callback)(int) = openCallback;
   popScreen();
   if (callback) callback(playlist);
}

static void initPlaylistScreen(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.selected = 0;
   state.noticeVisible = state.confirmDelete = 0;
   state.deleteTarget = -1;

   mode = pendingMode;
   pendingItem = pendingItemIn;
   openCallback = pendingCallback;

   font = openSystemFont(FONT_POP);
   int textW = LIST_WIDTH - 2 * ROW_PAD - ICON_SIZE - 12 - COUNT_GAP - 120;

   initLabel(&titleLabel, &font, MARGIN_X, HEADER_Y, state.screenW - 2 * MARGIN_X, AUTO, TITLE_SIZE,
             activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
             mode == MODE_CHOOSE ? "Add to playlist" : "Playlists");
   initLabel(&emptyLabel, &font, MARGIN_X + ROW_PAD, LIST_TOP + 8, state.screenW - 2 * MARGIN_X, AUTO, EMPTY_TEXT,
             activeTheme->textSecondary, TEXT_NOWRAP,
             "No playlists yet - press START to make one, or L3 on any video to file it straight into a new list.");
   initLabelRaw(&newLabel, &font, 0, 0, textW, AUTO, ROW_TEXT, activeTheme->accent, TEXT_NOWRAP_ELLIPSIS, "New playlist...");

   for (int i = 0; i < MAX_PLAYLISTS; i++) {
      initLabelRaw(&rowLabels[i],   &font, 0, 0, textW, AUTO, ROW_TEXT, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      initLabelRaw(&countLabels[i], &font, 0, 0, AUTO,  AUTO, ROW_TEXT, activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   initLabelRaw(&noticeLabel,     &font, 0, 0, state.screenW / 2, AUTO, NOTICE_TEXT, activeTheme->textPrimary,   TEXT_WRAP,   "");
   initLabelRaw(&noticeHintLabel, &font, 0, 0, AUTO, AUTO, NOTICE_HINT, activeTheme->textSecondary, TEXT_NOWRAP, "");

   initIcon(&rowIcon, ICON_DOCS, ICON_SIZE);
   initIcon(&newIcon, ICON_PLUS_CIRCLED, ICON_SIZE);

   initButtonHints(&hints, &font, state.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),  mode == MODE_CHOOSE ? "Add here" : "Open");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Back");
   if (mode == MODE_BROWSE) {
      addButtonHint(&hints, getConsoleGlyph(GLYPH_START),  "New");
      addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "Delete");
   }

   memset(&navigateRepeat, 0, sizeof navigateRepeat);
   refreshRows();
}

static void updatePlaylistScreen(void)
{
   if (oskInputActive()) return;

   if (state.noticeVisible) {   // modal: nothing underneath sees the pad until it is dismissed
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         state.noticeVisible = 0;
         if (state.confirmDelete) {
            state.confirmDelete = 0;
            deletePlaylist(state.deleteTarget);
            state.deleteTarget = -1;
            refreshRows();
         }
         return;
      }
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { state.noticeVisible = 0; state.confirmDelete = 0; }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }

   int rows = rowCount();
   if (rows > 0) {
      if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) state.selected++;
      else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   state.selected--;
      if (state.selected < 0)     state.selected = rows - 1;
      if (state.selected >= rows) state.selected = 0;
   }

   if (mode == MODE_BROWSE && isPadButtonPressed(PAD_BTN_START)) {
      oskInputBegin("Name the playlist", "", onNameTyped);
      return;
   }

   if (mode == MODE_BROWSE && isPadButtonPressed(PAD_BTN_SQUARE)) {
      int playlist = playlistForRow(state.selected);
      if (playlist >= 0 && playlist < getPlaylistCount()) {
         char message[160];
         snprintf(message, sizeof message, "Delete the playlist %s and its %d video(s)?",
                  getPlaylistName(playlist), getPlaylistItemCount(playlist));
         state.deleteTarget = playlist;
         showNotice(message, 1);
      }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CROSS)) activateRow();
}

static void drawRow(int row, int y)
{
   int isNewRow = (mode == MODE_CHOOSE && row == CHOOSE_NEW_ROW);
   int playlist = playlistForRow(row);
   int picked = (row == state.selected);

   fillGfxRectangle(MARGIN_X, y, LIST_WIDTH, ROW_H - 8, activeTheme->surface);
   if (picked) {
      fillGfxRectangle(MARGIN_X, y, LIST_WIDTH, ROW_H - 8, activeTheme->rowHighlight);
      fillGfxRectangle(MARGIN_X, y, 5, ROW_H - 8, activeTheme->accent);
   }

   uint32_t tint = picked ? activeTheme->accent : activeTheme->textSecondary;
   int textY = y + (ROW_H - 8 - ROW_TEXT) / 2 - 2;
   drawIcon(isNewRow ? &newIcon : &rowIcon, MARGIN_X + ROW_PAD, textY - 1, tint);

   int textX = MARGIN_X + ROW_PAD + ICON_SIZE + 12;
   if (isNewRow) {
      drawLabelAt(&newLabel, textX, textY);
      return;
   }
   setLabelColor(&rowLabels[playlist], picked ? activeTheme->textPrimary : activeTheme->textSecondary);
   drawLabelAt(&rowLabels[playlist], textX, textY);
   drawLabelAt(&countLabels[playlist],
               MARGIN_X + LIST_WIDTH - ROW_PAD - countLabels[playlist].tt.tex.w, textY);
}

static void drawPlaylistScreen(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);

   int rows = rowCount();
   if (rows == 0) drawLabel(&emptyLabel);
   for (int row = 0; row < rows; row++) drawRow(row, LIST_TOP + row * ROW_H);

   drawButtonHints(&hints, state.screenW);

   if (state.noticeVisible) {
      int panelW = state.screenW * 2 / 3;
      int panelH = noticeLabel.tt.tex.h + 2 * NOTICE_PAD + NOTICE_HINT + 18;
      int x = (state.screenW - panelW) / 2, y = (state.screenH - panelH) / 2;
      int ring = activeTheme->focusThickness;
      fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
      fillGfxRectangle(x - ring, y - ring, panelW + 2 * ring, panelH + 2 * ring, activeTheme->focusBorder);
      fillGfxRectangle(x, y, panelW, panelH, activeTheme->surface);
      drawLabelAt(&noticeLabel, x + NOTICE_PAD, y + NOTICE_PAD);
      drawLabelAt(&noticeHintLabel, x + NOTICE_PAD, y + panelH - NOTICE_PAD - NOTICE_HINT + 6);
   }
}

static void termPlaylistScreen(void)
{
   termButtonHints(&hints);
   freeIcon(&rowIcon);
   freeIcon(&newIcon);
   freeLabel(&titleLabel);
   freeLabel(&emptyLabel);
   freeLabel(&newLabel);
   freeLabel(&noticeLabel);
   freeLabel(&noticeHintLabel);
   for (int i = 0; i < MAX_PLAYLISTS; i++) { freeLabel(&rowLabels[i]); freeLabel(&countLabels[i]); }
   closeFont(&font);
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen playlistScreen = { initPlaylistScreen, NULL, updatePlaylistScreen, drawPlaylistScreen,
                                 NULL, termPlaylistScreen, SCREEN_TERMINATED };

void openPlaylists(void (*onOpen)(int playlist))
{
   pendingMode = MODE_BROWSE;
   pendingCallback = onOpen;
   memset(&pendingItemIn, 0, sizeof pendingItemIn);
   pushScreen(&playlistScreen);
}

void openPlaylistChooser(const SearchResult *item)
{
   pendingMode = MODE_CHOOSE;
   pendingCallback = NULL;
   pendingItemIn = *item;
   pushScreen(&playlistScreen);
}

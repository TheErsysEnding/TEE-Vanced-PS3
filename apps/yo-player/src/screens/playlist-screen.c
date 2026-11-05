// playlist screen - implementation (see playlist-screen.h). Laid out like search-input.c: a plain list of
// rows with a hint bar, which is the shape this app already uses for "pick one of these".

#include "screens/playlist-screen.h"
#include "storage.h"
#include "ui-hints.h"           // UiHints; brings ui-draw.h (UI_SAFE_*) and ui-fonts.h (uiInitLabel, metrics)
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

// Everything sits inside the safe area (UI_SAFE_X .. UI_SAFE_RIGHT, ui-draw.h) of the 1920x1080 canvas.
#define TITLE_BASELINE 107      // the page title's baseline, the same as on the list pages (Bild 4)
#define LIST_TOP      160
// The list never scrolls, so the pitch is what CHOOSE mode's 13 rows (12 lists + "New playlist...") allow:
// the last plate ends at y 958, clear of the hint row at 998. The 54-px plate still keeps 24-px text and its
// descenders well inside.
#define ROW_H         62
#define PLATE_H       (ROW_H - 8)
#define ROW_TEXT      24
#define ROW_WEIGHT    UI_WEIGHT_BOLD
#define ROW_PAD       18
#define COUNT_GAP     30
#define COUNT_W       120       // kept for the count; "60 videos" is about 92 px at 20/600
#define ICON_SIZE     26
#define ICON_GAP      12        // icon -> name
#define LIST_WIDTH    900
#define NOTICE_PAD    28
#define NOTICE_TEXT   24
#define NOTICE_GAP    18        // the message's last line -> the hint line under it
#define NOTICE_RADIUS 26        // the radius the row ring (UI_RING_ROW) round the box is made for

typedef enum { MODE_BROWSE, MODE_CHOOSE } PlaylistMode;

// In CHOOSE mode row 0 is "New playlist..." and the lists follow; in BROWSE mode the lists start at row 0
// and START makes a new one. Keeping the extra row out of BROWSE means the common case - open a list - has
// nothing in front of it.
#define CHOOSE_NEW_ROW 0

static Label       titleLabel, emptyLabel;
static Label       rowLabels[MAX_PLAYLISTS], countLabels[MAX_PLAYLISTS], newLabel;
static Label       noticeLabel, noticeHintLabel;
static Icon        rowIcon, newIcon;
static UiHints     hints;
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
   uiSetTextLabel(&noticeLabel, UI_BODY_WEIGHT, text);   // can quote a playlist name: external text
   setLabelText(&noticeHintLabel, isConfirm ? "X Delete     O Cancel" : "Press X to close");
   state.confirmDelete = isConfirm;
   state.noticeVisible = 1;
}

// rebuild the row labels; called on entry and after anything changes the set
static void refreshRows(void)
{
   int count = getPlaylistCount();
   for (int i = 0; i < count; i++) {
      // a name is whatever was typed into the keyboard, in any script: drawn raw, and in FONT_POP as a
      // whole when Manrope lacks any of its characters
      uiSetTextLabel(&rowLabels[i], ROW_WEIGHT, getPlaylistName(i));
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

   // Fonts are shared app-wide (ui-fonts.h): nothing is opened here and nothing may be closed in term.
   int textW = LIST_WIDTH - 2 * ROW_PAD - ICON_SIZE - ICON_GAP - COUNT_GAP - COUNT_W;

   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, UI_SAFE_X, 0, UI_SAFE_RIGHT - UI_SAFE_X, UI_PAGE_TITLE_SIZE,
               activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
               mode == MODE_CHOOSE ? "Add to playlist" : "Playlists");
   // one line at this size (about 930 px), but wrapped inside the safe area rather than clipped should a font
   // ever draw it wider - it was an unwrapped line that simply stopped at its width
   uiInitLabel(&emptyLabel, UI_BODY_WEIGHT, UI_SAFE_X + ROW_PAD, 0, UI_SAFE_RIGHT - UI_SAFE_X - ROW_PAD,
               UI_BODY_SIZE, activeTheme->textSecondary, TEXT_WRAP,
               "No playlists yet - press START to make one, or L3 on any video to file it straight into a new list.");
   uiInitLabel(&newLabel, ROW_WEIGHT, 0, 0, textW, ROW_TEXT, activeTheme->accent, TEXT_NOWRAP_ELLIPSIS, "New playlist...");

   for (int i = 0; i < MAX_PLAYLISTS; i++) {
      uiInitTextLabel(&rowLabels[i], ROW_WEIGHT,     0, 0, textW, ROW_TEXT,     activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      uiInitLabel(&countLabels[i],   UI_META_WEIGHT, 0, 0, AUTO,  UI_META_SIZE, activeTheme->textSecondary, TEXT_NOWRAP,          "");
   }
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, state.screenW / 2, NOTICE_TEXT, activeTheme->textPrimary,
                   TEXT_WRAP, "");
   uiInitLabel(&noticeHintLabel, UI_HINT_WEIGHT, 0, 0, AUTO, UI_HINT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");

   initIcon(&rowIcon, ICON_DOCS, ICON_SIZE);
   initIcon(&newIcon, ICON_PLUS_CIRCLED, ICON_SIZE);

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  mode == MODE_CHOOSE ? "Add here" : "Open");
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");
   if (mode == MODE_BROWSE) {
      uiAddHint(&hints, GLYPH_START,  "New");
      uiAddHint(&hints, GLYPH_SQUARE, "Delete");
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

// The baseline that centres row text in a plate starting at plateY, measured on Manrope. The name, its count
// and "New playlist..." all sit on it, so a name that had to be drawn in FONT_POP (Japanese, emoji ...) still
// lines up with the Manrope count beside it - the two fonts put their glyphs at different heights in a texture.
static int rowBaseline(int plateY)
{
   Font *font = uiGetFont(ROW_WEIGHT);
   return uiTextYForCenter(font, ROW_TEXT, plateY, PLATE_H) + uiGetTextMetrics(font, ROW_TEXT).baseline;
}

// an icon with its ink centred on the plate: drawIcon places the em box, and where the ink sits inside that
// box is up to the glyph
static void drawRowIcon(Icon *icon, int x, int plateY, uint32_t tint)
{
   if (!icon->tex) return;   // the glyph did not rasterise; drawIcon would skip it as well
   drawIcon(icon, x, plateY + (PLATE_H - icon->tex->h) / 2 - icon->offsetY, tint);
}

static void drawRow(int row, int y)
{
   int isNewRow = (mode == MODE_CHOOSE && row == CHOOSE_NEW_ROW);
   int playlist = playlistForRow(row);
   int picked = (row == state.selected);

   fillGfxRectangle(UI_SAFE_X, y, LIST_WIDTH, PLATE_H, activeTheme->surface);
   if (picked) {
      fillGfxRectangle(UI_SAFE_X, y, LIST_WIDTH, PLATE_H, activeTheme->rowHighlight);
      fillGfxRectangle(UI_SAFE_X, y, 5, PLATE_H, activeTheme->accent);
   }

   uint32_t tint = picked ? activeTheme->accent : activeTheme->textSecondary;
   drawRowIcon(isNewRow ? &newIcon : &rowIcon, UI_SAFE_X + ROW_PAD, y, tint);

   int baseline = rowBaseline(y);
   int textX = UI_SAFE_X + ROW_PAD + ICON_SIZE + ICON_GAP;
   if (isNewRow) {
      drawLabelAt(&newLabel, textX, uiLabelYForBaseline(&newLabel, baseline));
      return;
   }
   setLabelColor(&rowLabels[playlist], picked ? activeTheme->textPrimary : activeTheme->textSecondary);
   drawLabelAt(&rowLabels[playlist], textX, uiLabelYForBaseline(&rowLabels[playlist], baseline));
   drawLabelAt(&countLabels[playlist], UI_SAFE_X + LIST_WIDTH - ROW_PAD - countLabels[playlist].tt.tex.w,
               uiLabelYForBaseline(&countLabels[playlist], baseline));
}

static void drawPlaylistScreen(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   uiPlaceLabelBaseline(&titleLabel, UI_SAFE_X, TITLE_BASELINE);
   drawLabel(&titleLabel);

   int rows = rowCount();
   if (rows == 0) {
      // on the line where the first list will appear
      uiPlaceLabelBaseline(&emptyLabel, UI_SAFE_X + ROW_PAD, rowBaseline(LIST_TOP));
      drawLabel(&emptyLabel);
   }
   for (int row = 0; row < rows; row++) drawRow(row, LIST_TOP + row * ROW_H);

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);

   if (state.noticeVisible) {
      // The padding is measured from the ink, not from the textures (whose first and last rows are blank by
      // an amount that depends on the font): the message's capitals start NOTICE_PAD below the panel's top,
      // and the hint line's baseline sits NOTICE_PAD above its bottom.
      UiTextMetrics text = uiGetTextMetrics(noticeLabel.font, noticeLabel.size);
      UiTextMetrics hint = uiGetTextMetrics(noticeHintLabel.font, noticeHintLabel.size);
      int textH = noticeLabel.tt.tex.h - text.capTop;   // first line's capitals .. the last line's descenders
      if (textH < 0) textH = 0;
      int panelW = state.screenW * 2 / 3;
      int panelH = NOTICE_PAD + textH + NOTICE_GAP + (hint.baseline - hint.capTop) + NOTICE_PAD;
      int x = (state.screenW - panelW) / 2, y = (state.screenH - panelH) / 2;
      // the same box as home's and search's: a rounded panel, and the focus ring round it because it owns
      // the pad until it is answered
      fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
      uiFillRounded(x, y, panelW, panelH, NOTICE_RADIUS, activeTheme->surface);
      uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);
      drawLabelAt(&noticeLabel, x + NOTICE_PAD, y + NOTICE_PAD - text.capTop);
      drawLabelAt(&noticeHintLabel, x + NOTICE_PAD, uiLabelYForBaseline(&noticeHintLabel, y + panelH - NOTICE_PAD));
   }
}

static void termPlaylistScreen(void)
{
   uiTermHints(&hints);
   freeIcon(&rowIcon);
   freeIcon(&newIcon);
   freeLabel(&titleLabel);
   freeLabel(&emptyLabel);
   freeLabel(&newLabel);
   freeLabel(&noticeLabel);
   freeLabel(&noticeHintLabel);
   for (int i = 0; i < MAX_PLAYLISTS; i++) { freeLabel(&rowLabels[i]); freeLabel(&countLabels[i]); }
   // no closeFont: the fonts belong to ui-fonts.c and every other screen is still drawing with them
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

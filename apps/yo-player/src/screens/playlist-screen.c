// playlist screen - implementation (see playlist-screen.h). Laid out like search-input.c: a plain list of
// rows with a hint bar, which is the shape this app already uses for "pick one of these". The rows have the
// Ember settings look (Bild 3): no plate of their own, the focused one on a rounded plate - here with the
// focus ring and its glow, since nothing inside a row shows the focus the way a settings segment does.

#include "screens/playlist-screen.h"
#include "storage.h"
#include "sidebar.h"            // the rail, when the screen was opened from it: collapsed, or open over it
#include "screens/home.h"       // updateRailOverScreen: LEFT opens the rail here as on Home
#include "ui-hints.h"           // UiHints; brings ui-draw.h (UI_SAFE_*) and ui-fonts.h (uiInitLabel, metrics)
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "theme.h"
#include "anim.h"               // interpolateColor: the focused plate's colour
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
#include <sys/sys_time.h>       // sys_time_get_system_time: the move timer, restarted as the rail closes

// Everything sits inside the safe area of the 1920x1080 canvas. The content starts beside the collapsed rail
// (112 px), at x 176 like every Ember page - also when the screen opens as the chooser, which draws no rail:
// the list then sits where the page it was opened from has its content.
#define CONTENT_X      176
// the title's capitals centred on the band the list pages' header uses (home.c): its baseline comes to 107
// as in Bild 4, and it does not move when this screen opens over Home
#define HEADER_Y       67
#define HEADER_H       45
#define LIST_TOP       160
// The list never scrolls, so the pitch is what CHOOSE mode's 13 rows (12 lists + "New playlist...") allow:
// the last plate ends at y 958 and its ring at 967, clear of the hint row at 998. The 54-px plate still keeps
// 24-px text and its descenders well inside.
#define ROW_H          62
#define PLATE_H        (ROW_H - 8)
#define LIST_W         1152     // the settings rows' width (Bild 3): a count stays within reach of its name
#define ROW_TEXT       24
#define ROW_WEIGHT     UI_WEIGHT_BOLD
#define ROW_PAD        20       // plate edge -> icon, and count -> plate edge
#define COUNT_GAP      30
#define COUNT_W        120      // kept for the count; "60 videos" is about 92 px at 20/600
#define ICON_SIZE      26
#define ICON_GAP       14       // icon -> name
// The modal box is search.c's: a rounded panel two thirds of the screen wide with the focus ring round it,
// the message, and under it the buttons that answer it as a hint row with the console's own glyphs.
#define NOTICE_PAD_X      44
#define NOTICE_PAD_TOP    40    // panel top -> the message's first capitals
#define NOTICE_PAD_BOTTOM 30    // the hint row -> panel bottom
#define NOTICE_GAP        24    // the message -> the hint row
#define NOTICE_TEXT       24
#define NOTICE_RADIUS     26    // the radius the row ring (UI_RING_ROW) round the box is made for

typedef enum { MODE_BROWSE, MODE_CHOOSE } PlaylistMode;

// In CHOOSE mode row 0 is "New playlist..." and the lists follow; in BROWSE mode the lists start at row 0
// and START makes a new one. Keeping the extra row out of BROWSE means the common case - open a list - has
// nothing in front of it.
#define CHOOSE_NEW_ROW 0

static Label       titleLabel, emptyLabel;
static Label       rowLabels[MAX_PLAYLISTS], countLabels[MAX_PLAYLISTS], newLabel;
static Label       noticeLabel;
static Icon        rowIcon, newIcon;
static UiHints     hints;
static UiHints     closeHints, confirmHints;   // the notice's answers: X Close, or X Delete / O Cancel
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
   uint32_t plateFill;     // the focused plate: hintFill over the page, opaque (see initPlaylistScreen)
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
   // The focused plate is the settings page's hintFill, made opaque over the page: the glow goes down
   // under the plate, and through a see-through plate it would tint the whole row orange.
   state.plateFill = interpolateColor(activeTheme->appBg, uiWithAlpha(activeTheme->hintFill, 0xFF),
                                      (float)(activeTheme->hintFill >> 24) / 255.0f);

   mode = pendingMode;
   pendingItem = pendingItemIn;
   openCallback = pendingCallback;

   // Fonts are shared app-wide (ui-fonts.h): nothing is opened here and nothing may be closed in term.
   int textW = LIST_W - 2 * ROW_PAD - ICON_SIZE - ICON_GAP - COUNT_GAP - COUNT_W;

   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_PAGE_TITLE_SIZE,
               activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS,
               mode == MODE_CHOOSE ? "Add to playlist" : "Playlists");
   // one line at this size (about 930 px), but wrapped inside the safe area rather than clipped should a font
   // ever draw it wider - it was an unwrapped line that simply stopped at its width
   uiInitLabel(&emptyLabel, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE,
               activeTheme->textSecondary, TEXT_WRAP,
               "No playlists yet - press START to make one, or L3 on any video to file it straight into a new list.");
   // the one row that makes something rather than opens it, in the accent's text colour
   uiInitLabel(&newLabel, ROW_WEIGHT, 0, 0, textW, ROW_TEXT, activeTheme->accentText, TEXT_NOWRAP_ELLIPSIS, "New playlist...");

   for (int i = 0; i < MAX_PLAYLISTS; i++) {
      uiInitTextLabel(&rowLabels[i], ROW_WEIGHT,     0, 0, textW, ROW_TEXT,     activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
      uiInitLabel(&countLabels[i],   UI_META_WEIGHT, 0, 0, AUTO,  UI_META_SIZE, activeTheme->textSecondary, TEXT_NOWRAP,          "");
   }
   uiInitTextLabel(&noticeLabel, UI_BODY_WEIGHT, 0, 0, state.screenW * 2 / 3 - 2 * NOTICE_PAD_X, NOTICE_TEXT,
                   activeTheme->textPrimary, TEXT_WRAP, "");
   // the notice's own line: the console's buttons, where the old text spelled "Press X to close"
   uiInitHints(&closeHints);
   uiAddHint(&closeHints, GLYPH_CROSS, "Close");
   uiInitHints(&confirmHints);
   uiAddHint(&confirmHints, GLYPH_CROSS,  "Delete");
   uiAddHint(&confirmHints, GLYPH_CIRCLE, "Cancel");

   initIcon(&rowIcon, ICON_DOCS, ICON_SIZE);
   initIcon(&newIcon, ICON_PLUS_CIRCLED, ICON_SIZE);

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  mode == MODE_CHOOSE ? "Add here" : "Open");
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");
   if (mode == MODE_BROWSE) {
      uiAddHint(&hints, GLYPH_START,  "New");
      uiAddHint(&hints, GLYPH_SQUARE, "Delete");
      uiAddHint(&hints, GLYPH_DPAD_LEFT, "Menu");   // opened from the rail, the rail opens here as on Home
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

   // Opened from the rail, LEFT opens the rail here as on Home (home.h); Playlists picked in it is this screen,
   // so it just closes, and a direction still held then waits out the normal delay. The "add to playlist"
   // chooser is a step inside another page and has no rail: LEFT does nothing there, as before.
   if (mode == MODE_BROWSE) {
      int rail = updateRailOverScreen(SIDE_PLAYLISTS);
      if (rail == RAIL_GONE) return;   // Home is back on top: this screen is gone, touch nothing of it
      if (rail != RAIL_NOT_OPEN) {
         if (!isSidebarFocused()) { navigateRepeat.timer = sys_time_get_system_time(); navigateRepeat.repeats = 0; }
         return;
      }
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

// The focused row's plate: the glow first, so it shows round the plate and not over it, then the plate and
// the ring. The ring is the one made for radius-16 cards (UI_RING_CARD, 5 px out): round a radius-18 plate
// it keeps that distance along the sides and comes out a pixel wider at the corners, where the search rows'
// ring (radius 26, 4 px out) would almost touch them.
static void drawFocusPlate(int y)
{
   uiDrawFocusGlow(CONTENT_X, y, LIST_W, PLATE_H, activeTheme->focusGlow);
   uiFillRounded(CONTENT_X, y, LIST_W, PLATE_H, UI_RADIUS_ROW, state.plateFill);
   uiDrawFocusRing(CONTENT_X, y, LIST_W, PLATE_H, UI_RING_CARD, activeTheme->focusBorder);
}

static void drawRow(int row, int y)
{
   int isNewRow = (mode == MODE_CHOOSE && row == CHOOSE_NEW_ROW);
   int playlist = playlistForRow(row);
   int picked = (row == state.selected);

   uint32_t tint = picked ? activeTheme->accent : isNewRow ? activeTheme->accentText : activeTheme->textSecondary;
   drawRowIcon(isNewRow ? &newIcon : &rowIcon, CONTENT_X + ROW_PAD, y, tint);

   int baseline = rowBaseline(y);
   int textX = CONTENT_X + ROW_PAD + ICON_SIZE + ICON_GAP;
   if (isNewRow) {
      uiDrawLabelAt(&newLabel, textX, uiLabelYForBaseline(&newLabel, baseline));
      return;
   }
   uiDrawLabelAt(&rowLabels[playlist], textX, uiLabelYForBaseline(&rowLabels[playlist], baseline));
   uiDrawLabelAt(&countLabels[playlist], CONTENT_X + LIST_W - ROW_PAD - countLabels[playlist].tt.tex.w,
                 uiLabelYForBaseline(&countLabels[playlist], baseline));
}

// The modal box, laid out from its message's measurements, so a message in FONT_POP (a Japanese playlist
// name) sits exactly where a Manrope one would: its capitals NOTICE_PAD_TOP below the panel's top, the
// answers NOTICE_GAP under its texture, and NOTICE_PAD_BOTTOM under them.
static void drawNotice(void)
{
   UiTextMetrics metrics = uiGetTextMetrics(noticeLabel.font, noticeLabel.size);
   int textTop = NOTICE_PAD_TOP - metrics.capTop;
   if (textTop < 0) textTop = 0;
   const UiHints *answers = state.confirmDelete ? &confirmHints : &closeHints;
   int panelW = state.screenW * 2 / 3;
   int panelH = textTop + noticeLabel.tt.tex.h + NOTICE_GAP + UI_HINT_GLYPH_H + NOTICE_PAD_BOTTOM;
   int x = (state.screenW - panelW) / 2, y = (state.screenH - panelH) / 2;
   // the same box as home's and search's: a rounded panel, and the focus ring round it because it owns
   // the pad until it is answered
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
   uiFillRounded(x, y, panelW, panelH, NOTICE_RADIUS, activeTheme->surface);
   uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);
   uiDrawLabelAt(&noticeLabel, x + NOTICE_PAD_X, y + textTop);
   uiDrawHints(answers, x + NOTICE_PAD_X, y + textTop + noticeLabel.tt.tex.h + NOTICE_GAP, UI_ALIGN_LEFT);
}

static void drawPlaylistScreen(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   // From the rail the screen is one of its pages, and the rail says so. As the chooser (L3 on a video) it is
   // a step inside the page it came from, which the rail cannot mark.
   if (mode == MODE_BROWSE && !isSidebarFocused()) drawSidebarPassive(state.screenH, SIDE_PLAYLISTS);

   int rows = rowCount();
   if (rows > 0) drawFocusPlate(LIST_TOP + state.selected * ROW_H);
   for (int row = 0; row < rows; row++) drawRow(row, LIST_TOP + row * ROW_H);
   if (rows == 0) {
      // on the line where the first list will appear
      uiPlaceLabelBaseline(&emptyLabel, CONTENT_X, rowBaseline(LIST_TOP));
      drawLabel(&emptyLabel);
   }

   // the title after the list: the first row's glow reaches up under it, and text drawn before the glow
   // would come out tinted orange
   uiPlaceLabelCentered(&titleLabel, CONTENT_X, HEADER_Y, HEADER_H);
   drawLabel(&titleLabel);

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   // the rail, open, over everything - the hints included, dimmed by its scrim with the rest
   if (mode == MODE_BROWSE && isSidebarFocused()) drawSidebar(state.screenH);

   if (state.noticeVisible) drawNotice();
}

static void termPlaylistScreen(void)
{
   uiTermHints(&hints);
   uiTermHints(&closeHints);
   uiTermHints(&confirmHints);
   freeIcon(&rowIcon);
   freeIcon(&newIcon);
   freeLabel(&titleLabel);
   freeLabel(&emptyLabel);
   freeLabel(&newLabel);
   freeLabel(&noticeLabel);
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

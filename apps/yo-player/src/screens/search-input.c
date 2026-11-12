// search-input screen - the app's own search field plus the search history (see search-input.h).
//
// The page is the top of Bild 5 (Ember): the search field across the top and, under "Recent searches", the
// history - as rows, one query each, not as Bild 5's chips: the history is walked with UP/DOWN, and a row of
// chips would make DOWN mean "the next one to the right". The collapsed rail sits at the left edge, drawn
// passive: this screen is pushed and Circle is Back here, so the rail only says where you are.

#include "screens/search-input.h"
#include "storage.h"
#include "sidebar.h"            // the rail: collapsed, or open over the screen while it has the focus
#include "screens/home.h"       // updateRailOverScreen: LEFT opens the rail here as on Home
#include "ui-hints.h"           // the Ember hint row
#include "ui-draw.h"            // safe area, rounded fills, focus glow and ring, uiDrawLabelAt
#include "ui-fonts.h"           // the shared fonts, the type scale, label helpers and text placement
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"       // the field's magnifier, the rows' clock

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <sys/sys_time.h>       // sys_time_get_system_time: the move timer, restarted as the rail closes

// Content runs from beside the collapsed rail (112 px wide) to the safe area's right edge - x 176..1824,
// the column every Ember page uses. The field and the rows span all of it, so their right ends line up with
// the results screen's (search.c).
#define CONTENT_X       SIDEBAR_CONTENT_X
#define CONTENT_RIGHT   UI_SAFE_RIGHT
#define CONTENT_W       (CONTENT_RIGHT - CONTENT_X)
// The field as Bild 5 draws it: 88 tall at the top of the page, radius 26 (the radius the row ring,
// UI_RING_ROW, is made for) with a 2-px inner border; the magnifier's 34-px box 30 in from the left and the
// text 88 in (30 + 34 + 22 + the input's 2). The rows below take the same two columns, so every icon sits
// under the magnifier and every query under the field's text.
#define FIELD_Y         58
#define FIELD_H         88
#define FIELD_RADIUS    26
#define FIELD_BORDER    2       // its inner border: hairline over the field colour
#define FIELD_PAD       30
#define ICON_BOX        34
#define FIELD_ICON_SIZE 28      // the icon font's magnifier at 28 px is about 28 px of ink, as drawn in Bild 5
#define TEXT_X          (CONTENT_X + 88)
#define TEXT_W          (CONTENT_RIGHT - FIELD_PAD - TEXT_X)
#define FIELD_TEXT      34
#define FIELD_WEIGHT    UI_WEIGHT_BOLD
#define PLACEHOLDER_GAP 10      // "Press" - the cross - "to type"
#define RECENT_BASELINE 201     // capitals centred on the band where the results screen has its sort segment
                                // (y 166..221), Bild 5's row of "Recent" and the segment
#define RECENT_TEXT     20      // "Recent" in Bild 5: 20/700 in text-3
#define LIST_TOP        230
#define ROW_PLATE_H     60
#define ROW_GAP         16      // between two plates; a focused row's ring takes 9 of it
#define ROW_H           (ROW_PLATE_H + ROW_GAP)
#define ROW_RADIUS      14      // the radius the thumbnail ring (UI_RING_THUMB) is made for
#define ROW_ICON_SIZE   24      // the clock Bild 5's recent chips carry, a little larger on a 60-px row
#define ROW_TEXT        24
#define ROW_WEIGHT      UI_WEIGHT_BOLD
#define LIST_BOTTOM     (UI_HINTS_BOTTOM_Y - 18)   // the last plate ends here: its ring 9 px lower, then the hints

// row 0 is the field itself; 1..historyCount are the remembered queries
#define ROW_FIELD 0
#define CROSS_HINT  0   // X's slot in the hint row (see initSearchInput): retitled per focused row
#define SQUARE_HINT 2   // Square's slot: shown only on a remembered query, the one kind of row it deletes

static struct {
   char  query[SEARCH_QUERY_LEN];
   char  history[MAX_SEARCH_HISTORY][SEARCH_QUERY_LEN];
   int   historyCount;
   int   selected;        // 0 = the field, else history[selected - 1]
   int   scroll;          // first history row drawn
   int   visibleRows;
   int   screenW, screenH;
} state;

static SearchInputDone doneCallback;
static char            pendingInitial[SEARCH_QUERY_LEN];

static Label        fieldLabel, pressLabel, toTypeLabel, idleLabel, recentLabel, emptyLabel;
static Label        rowLabels[MAX_SEARCH_HISTORY];
static Icon         fieldIcon, rowIcon;
static UiHints      hints;
static ButtonRepeat navigateRepeat;

static void reloadHistory(void)
{
   state.historyCount = getSearchHistory(state.history, MAX_SEARCH_HISTORY);
   // queries are typed text: drawn raw, and in FONT_POP when Manrope lacks one of their characters
   for (int i = 0; i < state.historyCount; i++) uiSetTextLabel(&rowLabels[i], ROW_WEIGHT, state.history[i]);
   if (state.selected > state.historyCount) state.selected = state.historyCount;
}

// An empty field shows a placeholder instead, dimmed so it cannot be mistaken for a query: "Press X to type"
// with the console's own cross while the field has the focus (drawPlaceholder), "Search YouTube" while a
// remembered query has it - X then searches that query, so the field must not promise typing.
static void setFieldText(const char *text)
{
   strCopy(state.query, sizeof state.query, text ? text : "");
   uiSetTextLabel(&fieldLabel, FIELD_WEIGHT, state.query);
}

// hand the query back to whoever opened the screen. The screen is gone by the time the callback runs, so
// nothing here may touch its state afterwards - the query lives in a file-scope buffer for exactly that.
static void confirm(const char *query)
{
   static char chosen[SEARCH_QUERY_LEN];
   if (!query || !query[0]) return;
   strCopy(chosen, sizeof chosen, query);
   addSearchHistory(chosen);

   SearchInputDone done = doneCallback;
   popScreen();
   if (done) done(chosen);
}

static void onTyped(const char *text)
{
   if (!text || !text[0]) return;
   confirm(text);
}

// X types on the field and searches a remembered query straight away, so its caption follows the focused
// row; a fixed "Search" stood under a field where X opens the keyboard. Square deletes a remembered query
// and does nothing on the field, so its hint is there only while a query has the focus - "Delete" under
// the field promised an action that row does not have. Renders only when the row changes from the field to
// a query or back.
static void updateRowHints(void)
{
   int onQuery = state.selected != ROW_FIELD;
   uiSetHintCaption(&hints, CROSS_HINT, onQuery ? "Search" : "Type");
   uiSetHintShown(&hints, SQUARE_HINT, onQuery);
}

static void keepSelectionVisible(void)
{
   if (state.selected == ROW_FIELD) { state.scroll = 0; return; }
   int row = state.selected - 1;
   if (row < state.scroll)                          state.scroll = row;
   else if (row >= state.scroll + state.visibleRows) state.scroll = row - state.visibleRows + 1;
   if (state.scroll < 0) state.scroll = 0;
}

static void initSearchInput(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.selected = ROW_FIELD;
   state.scroll = 0;

   // as many whole plates as fit above LIST_BOTTOM: the last one needs no gap under it
   state.visibleRows = (LIST_BOTTOM - LIST_TOP + ROW_GAP) / ROW_H;
   if (state.visibleRows < 1) state.visibleRows = 1;

   // The fonts are the shared ones (ui-fonts.h) - nothing here opens or closes one. Labels are positioned
   // when they are drawn, by their measured capitals: a query may come out in FONT_POP, which sits at another
   // height in its texture than Manrope.
   uiInitTextLabel(&fieldLabel,  FIELD_WEIGHT, 0, 0, TEXT_W, FIELD_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&pressLabel,      FIELD_WEIGHT, 0, 0, AUTO, FIELD_TEXT, activeTheme->textTertiary, TEXT_NOWRAP, "Press");
   uiInitLabel(&toTypeLabel,     FIELD_WEIGHT, 0, 0, AUTO, FIELD_TEXT, activeTheme->textTertiary, TEXT_NOWRAP, "to type");
   uiInitLabel(&idleLabel,       FIELD_WEIGHT, 0, 0, AUTO, FIELD_TEXT, activeTheme->textTertiary, TEXT_NOWRAP, "Search YouTube");
   uiInitLabel(&recentLabel,     UI_WEIGHT_BOLD, 0, 0, AUTO, RECENT_TEXT, activeTheme->textTertiary, TEXT_NOWRAP, "Recent searches");
   uiInitLabel(&emptyLabel,      UI_BODY_WEIGHT, 0, 0, AUTO, UI_BODY_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Nothing yet - your searches will be listed here");
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++)
      uiInitTextLabel(&rowLabels[i], ROW_WEIGHT, 0, 0, TEXT_W, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initIcon(&fieldIcon, ICON_SEARCH, FIELD_ICON_SIZE);
   initIcon(&rowIcon,   ICON_CLOCK,  ROW_ICON_SIZE);

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "Type");     // CROSS_HINT: "Search" on a remembered query (updateRowHints)
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");
   uiAddHint(&hints, GLYPH_SQUARE, "Delete");   // SQUARE_HINT: only on a remembered query (updateRowHints)
   uiAddHint(&hints, GLYPH_START,  "Type");
   uiAddHint(&hints, GLYPH_DPAD_LEFT, "Menu");   // the rail opens here as on Home

   setFieldText(pendingInitial);
   reloadHistory();
   updateRowHints();
}

static void updateSearchInput(void)
{
   if (oskInputActive()) return;

   // The rail: LEFT opens it here as on Home (home.h). Search picked in it is this screen: the rail just closes.
   // A direction still held as it closes waits out the normal delay instead of moving at once.
   int rail = updateRailOverScreen(SIDE_SEARCH);
   if (rail == RAIL_GONE) return;   // Home is back on top: this screen is gone, touch nothing of it
   if (rail != RAIL_NOT_OPEN) {
      if (!isSidebarFocused()) { navigateRepeat.timer = sys_time_get_system_time(); navigateRepeat.repeats = 0; }
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }
   if (isPadButtonPressed(PAD_BTN_START))  { oskInputBegin("Search YouTube", state.query, onTyped); return; }

   int last = state.historyCount;   // row index of the last entry (0 when there is no history)
   if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) state.selected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   state.selected--;
   if (state.selected < 0)    state.selected = 0;
   if (state.selected > last) state.selected = last;
   keepSelectionVisible();

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      // on the field: type. on a remembered query: search it straight away.
      if (state.selected == ROW_FIELD) oskInputBegin("Search YouTube", state.query, onTyped);
      else                             confirm(state.history[state.selected - 1]);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_SQUARE) && state.selected != ROW_FIELD) {
      removeSearchHistory(state.history[state.selected - 1]);
      reloadHistory();
      if (state.selected > state.historyCount) state.selected = state.historyCount;
      keepSelectionVisible();
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

// A button as the hint row draws it (V3.0.0: a round chip or a key cap, see ui-hints.h). Returns its width.
static int drawGlyph(ConsoleGlyph glyph, int x, int y, int height)
{
   return uiDrawButtonGlyph(glyph, x, y, height, activeTheme->hintFill);
}

// "Press [X] to type" in the empty field, dimmed to text-3 like any placeholder, with the console's own
// cross where the old text spelled out the letter X
static void drawPlaceholder(void)
{
   int x = TEXT_X;
   uiDrawLabelAt(&pressLabel, x, uiLabelYForCenter(&pressLabel, FIELD_Y, FIELD_H));
   x += pressLabel.tt.tex.w + PLACEHOLDER_GAP;
   x += drawGlyph(GLYPH_CROSS, x, FIELD_Y + (FIELD_H - UI_HINT_GLYPH_H) / 2, UI_HINT_GLYPH_H) + PLACEHOLDER_GAP;
   uiDrawLabelAt(&toTypeLabel, x, uiLabelYForCenter(&toTypeLabel, FIELD_Y, FIELD_H));
}

static int historyRowY(int row) { return LIST_TOP + (row - state.scroll) * ROW_H; }

// A remembered query: a surface-1 plate with the clock of Bild 5's recent chips and the query beside it. The
// row in focus lifts to surface-2 and its clock turns orange; the ring and glow round it are drawn by the
// caller, the glow before every plate and the ring after.
static void drawHistoryRow(int row, int focused)
{
   int y = historyRowY(row);
   uiFillRounded(CONTENT_X, y, CONTENT_W, ROW_PLATE_H, ROW_RADIUS, focused ? activeTheme->surface : activeTheme->surface1);
   drawIconInBox(&rowIcon, CONTENT_X + FIELD_PAD, y, ICON_BOX, ROW_PLATE_H,
                 focused ? activeTheme->accent : activeTheme->textSecondary);
   uiDrawLabelAt(&rowLabels[row], TEXT_X, uiLabelYForCenter(&rowLabels[row], y, ROW_PLATE_H));
}

static void drawSearchInput(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);

   int end = state.scroll + state.visibleRows;   // one past the last history row drawn
   if (end > state.historyCount) end = state.historyCount;
   int fieldFocused = state.selected == ROW_FIELD;
   int focusRow = state.selected - 1;            // the history row in focus (-1: the field has it)
   int rowFocused = !fieldFocused && focusRow >= state.scroll && focusRow < end;

   // The focus glow goes down first, under everything: the field and the plates drawn after it cover the
   // parts that fall on them, so it shows round the focused one and in the gaps - never over a neighbour's text.
   if (fieldFocused)    uiDrawFocusGlow(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H, activeTheme->focusGlow);
   else if (rowFocused) uiDrawFocusGlow(CONTENT_X, historyRowY(focusRow), CONTENT_W, ROW_PLATE_H, activeTheme->focusGlow);

   // the field: the magnifier turns orange while the field has the focus, like the rows' clocks
   fillField(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H);
   drawIconInBox(&fieldIcon, CONTENT_X + FIELD_PAD, FIELD_Y, ICON_BOX, FIELD_H,
                 fieldFocused ? activeTheme->accent : activeTheme->textSecondary);
   if (state.query[0])     uiDrawLabelAt(&fieldLabel, TEXT_X, uiLabelYForCenter(&fieldLabel, FIELD_Y, FIELD_H));
   else if (fieldFocused)  drawPlaceholder();
   else                    uiDrawLabelAt(&idleLabel, TEXT_X, uiLabelYForCenter(&idleLabel, FIELD_Y, FIELD_H));

   for (int row = state.scroll; row < end; row++) drawHistoryRow(row, rowFocused && row == focusRow);

   // the ring over the shape it marks
   if (fieldFocused)    uiDrawFocusRing(CONTENT_X, FIELD_Y, CONTENT_W, FIELD_H, UI_RING_ROW, activeTheme->focusBorder);
   else if (rowFocused) uiDrawFocusRing(CONTENT_X, historyRowY(focusRow), CONTENT_W, ROW_PLATE_H, UI_RING_THUMB, activeTheme->focusBorder);

   // the labels that sit on no shape come last, so the glow spreading under them never tints them
   uiDrawLabelAt(&recentLabel, CONTENT_X, uiLabelYForBaseline(&recentLabel, RECENT_BASELINE));
   if (state.historyCount == 0) uiDrawLabelAt(&emptyLabel, CONTENT_X, uiLabelYForCenter(&emptyLabel, LIST_TOP, ROW_PLATE_H));

   if (!isSidebarFocused()) drawSidebarPassive(state.screenH, SIDE_SEARCH);
   updateRowHints();    // here, after this frame's update: a deletion can hand the focus back to the field
   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   if (isSidebarFocused()) drawSidebar(state.screenH);   // open: over everything, the hints dimmed with the rest
}

static void termSearchInput(void)
{
   uiTermHints(&hints);
   freeLabel(&fieldLabel);
   freeLabel(&pressLabel);
   freeLabel(&toTypeLabel);
   freeLabel(&idleLabel);
   freeLabel(&recentLabel);
   freeLabel(&emptyLabel);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) freeLabel(&rowLabels[i]);
   freeIcon(&fieldIcon);   // the textures belong to the icon font's shared cache; this only lets go of them
   freeIcon(&rowIcon);
   // no closeFont: the fonts are shared, and closing one would blank every label on every screen
}

void openSearchInput(const char *initialText, SearchInputDone onDone)
{
   strCopy(pendingInitial, sizeof pendingInitial, initialText ? initialText : "");
   doneCallback = onDone;
   pushScreen(&searchInputScreen);
}

Screen searchInputScreen = { initSearchInput, NULL, updateSearchInput, drawSearchInput, NULL, termSearchInput, SCREEN_TERMINATED };

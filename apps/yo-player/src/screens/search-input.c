// search-input screen - the app's own search field plus the search history (see search-input.h).

#include "screens/search-input.h"
#include "storage.h"
#include "ui-hints.h"           // the Ember hint row
#include "ui-draw.h"            // safe area, rounded fills, focus glow and ring, uiDrawLabelAt
#include "ui-fonts.h"           // the shared fonts, the type scale, label helpers and text placement
#include "ui/console-glyphs.h"

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

// The field and the rows keep 9 px inside the safe area on both sides: that is how far their focus rings
// reach outside them (8 for the field's, 9 for a row's), so the rings stay inside it too. The title and the
// "Recent searches" line start where the shapes do, as on the results screen (search.c).
#define CONTENT_X       (UI_SAFE_X + 9)
#define CONTENT_W       (UI_SAFE_RIGHT - UI_SAFE_X - 2 * 9)
// The title's capitals sit on the line Home and the results screen give their headings (centred on the band
// of a top-right hint row, UI_HINTS_TOP_Y), so the heading does not jump from one screen to the next.
#define HEADER_Y        UI_HINTS_TOP_Y
#define HEADER_H        UI_HINT_GLYPH_H
#define FIELD_Y         140
#define FIELD_H         88      // the Ember search field (Bild 5): 88 tall, radius 26, the query in 34/700
#define FIELD_RADIUS    26      // the radius the row ring (UI_RING_ROW) is made for
#define FIELD_BORDER    2       // its inner border: hairline over the field colour
#define FIELD_TEXT      34
#define FIELD_WEIGHT    UI_WEIGHT_BOLD
#define FIELD_PAD       30
#define RECENT_BASELINE 274
#define RECENT_TEXT     20      // "Recent" in Bild 5: 20/700 in text-3
#define LIST_TOP        300
#define ROW_H           60      // row pitch: the plate plus the gap the focus ring needs, 9 px each side of it
#define ROW_PLATE_H     46
#define ROW_RADIUS      14      // the radius the thumbnail ring (UI_RING_THUMB) is made for
#define ROW_TEXT        24
#define ROW_WEIGHT      UI_WEIGHT_BOLD
#define ROW_PAD         20
#define EMPTY_BASELINE  (LIST_TOP + 31)
#define LIST_BOTTOM_GAP 24      // clearance kept between the last row and the hint row

// row 0 is the field itself; 1..historyCount are the remembered queries
#define ROW_FIELD 0

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

static Label        titleLabel, fieldLabel, recentLabel, emptyLabel;
static Label        rowLabels[MAX_SEARCH_HISTORY];
static UiHints      hints;
static ButtonRepeat navigateRepeat;

static void reloadHistory(void)
{
   state.historyCount = getSearchHistory(state.history, MAX_SEARCH_HISTORY);
   // queries are typed text: drawn raw, and in FONT_POP when Manrope lacks one of their characters
   for (int i = 0; i < state.historyCount; i++) uiSetTextLabel(&rowLabels[i], ROW_WEIGHT, state.history[i]);
   if (state.selected > state.historyCount) state.selected = state.historyCount;
}

static void setFieldText(const char *text)
{
   strCopy(state.query, sizeof state.query, text ? text : "");
   uiSetTextLabel(&fieldLabel, FIELD_WEIGHT, state.query[0] ? state.query : "Press X to type");
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

   int listHeight = UI_HINTS_BOTTOM_Y - LIST_BOTTOM_GAP - LIST_TOP;
   state.visibleRows = listHeight / ROW_H;
   if (state.visibleRows < 1) state.visibleRows = 1;

   // The fonts are the shared ones (ui-fonts.h) - nothing here opens or closes one. Labels are positioned
   // when they are drawn, by their measured capitals: a query may come out in FONT_POP, which sits at another
   // height in its texture than Manrope.
   int textWidth = CONTENT_W;
   uiInitLabel(&titleLabel,      UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Search");
   uiInitTextLabel(&fieldLabel,  FIELD_WEIGHT, 0, 0, textWidth - 2 * FIELD_PAD, FIELD_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&recentLabel,     UI_WEIGHT_BOLD, CONTENT_X, 0, AUTO, RECENT_TEXT, activeTheme->textTertiary, TEXT_NOWRAP, "Recent searches");
   uiInitLabel(&emptyLabel,      UI_BODY_WEIGHT, CONTENT_X, 0, AUTO, UI_BODY_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Nothing yet - your searches will be listed here");
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++)
      uiInitTextLabel(&rowLabels[i], ROW_WEIGHT, 0, 0, textWidth - 2 * ROW_PAD, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "Search");
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");
   uiAddHint(&hints, GLYPH_SQUARE, "Delete");
   uiAddHint(&hints, GLYPH_START,  "Type");

   setFieldText(pendingInitial);
   reloadHistory();
}

static void updateSearchInput(void)
{
   if (oskInputActive()) return;

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

static int historyRowY(int row) { return LIST_TOP + (row - state.scroll) * ROW_H; }

static void drawSearchInput(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);

   int fieldW = CONTENT_W;
   int end = state.scroll + state.visibleRows;   // one past the last history row drawn
   if (end > state.historyCount) end = state.historyCount;
   int focusRow = state.selected - 1;            // the history row in focus (-1: the field has it)
   int rowFocused = state.selected != ROW_FIELD && focusRow >= state.scroll && focusRow < end;

   // The focus glow goes down first, under everything: the field and the rows drawn after it cover the parts
   // that fall on them, so it shows round the focused one and in the gaps - never over a neighbour's text.
   if (state.selected == ROW_FIELD) uiDrawFocusGlow(CONTENT_X, FIELD_Y, fieldW, FIELD_H, activeTheme->focusGlow);
   else if (rowFocused)             uiDrawFocusGlow(CONTENT_X, historyRowY(focusRow), fieldW, ROW_PLATE_H, activeTheme->focusGlow);

   // the field
   fillField(CONTENT_X, FIELD_Y, fieldW, FIELD_H);
   uiDrawLabelAt(&fieldLabel, CONTENT_X + FIELD_PAD, uiLabelYForCenter(&fieldLabel, FIELD_Y, FIELD_H));

   for (int row = state.scroll; row < end; row++) {
      int y = historyRowY(row);
      uiFillRounded(CONTENT_X, y, fieldW, ROW_PLATE_H, ROW_RADIUS, activeTheme->surface);
      uiDrawLabelAt(&rowLabels[row], CONTENT_X + ROW_PAD, uiLabelYForCenter(&rowLabels[row], y, ROW_PLATE_H));
   }

   // the ring over the shape it marks
   if (state.selected == ROW_FIELD) uiDrawFocusRing(CONTENT_X, FIELD_Y, fieldW, FIELD_H, UI_RING_ROW, activeTheme->focusBorder);
   else if (rowFocused)             uiDrawFocusRing(CONTENT_X, historyRowY(focusRow), fieldW, ROW_PLATE_H, UI_RING_THUMB, activeTheme->focusBorder);

   // the labels that sit on no shape come last, so the glow spreading under them never tints them
   uiDrawLabelAt(&titleLabel, CONTENT_X, uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H));
   uiDrawLabelAt(&recentLabel, CONTENT_X, uiLabelYForBaseline(&recentLabel, RECENT_BASELINE));
   if (state.historyCount == 0) uiDrawLabelAt(&emptyLabel, CONTENT_X, uiLabelYForBaseline(&emptyLabel, EMPTY_BASELINE));

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
}

static void termSearchInput(void)
{
   uiTermHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&fieldLabel);
   freeLabel(&recentLabel);
   freeLabel(&emptyLabel);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) freeLabel(&rowLabels[i]);
   // no closeFont: the fonts are shared, and closing one would blank every label on every screen
}

void openSearchInput(const char *initialText, SearchInputDone onDone)
{
   strCopy(pendingInitial, sizeof pendingInitial, initialText ? initialText : "");
   doneCallback = onDone;
   pushScreen(&searchInputScreen);
}

Screen searchInputScreen = { initSearchInput, NULL, updateSearchInput, drawSearchInput, NULL, termSearchInput, SCREEN_TERMINATED };

// search-input screen - the app's own search field plus the search history (see search-input.h).

#include "screens/search-input.h"
#include "storage.h"
#include "ui/button-hints.h"
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

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define FIELD_Y       84
#define FIELD_H       62
#define FIELD_TEXT    28
#define FIELD_PAD     18
#define RECENT_Y      170
#define RECENT_TEXT   21
#define LIST_TOP      206
#define ROW_H         46
#define ROW_TEXT      24
#define ROW_PAD       12
#define HINTS_BOTTOM  42
#define HINT_GLYPH_H  25
#define HINT_TEXT     21
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

static Font        font;
static Label       titleLabel, fieldLabel, recentLabel, emptyLabel;
static Label       rowLabels[MAX_SEARCH_HISTORY];
static ButtonHints hints;
static ButtonRepeat navigateRepeat;

static void reloadHistory(void)
{
   state.historyCount = getSearchHistory(state.history, MAX_SEARCH_HISTORY);
   for (int i = 0; i < state.historyCount; i++) setLabelText(&rowLabels[i], state.history[i]);
   if (state.selected > state.historyCount) state.selected = state.historyCount;
}

static void setFieldText(const char *text)
{
   strCopy(state.query, sizeof state.query, text ? text : "");
   setLabelText(&fieldLabel, state.query[0] ? state.query : "Press X to type");
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

   int listHeight = state.screenH - HINTS_BOTTOM - LIST_BOTTOM_GAP - LIST_TOP;
   state.visibleRows = listHeight / ROW_H;
   if (state.visibleRows < 1) state.visibleRows = 1;

   font = openSystemFont(FONT_POP);
   int textWidth = state.screenW - 2 * MARGIN_X;
   initLabel(&titleLabel,  &font, MARGIN_X, HEADER_Y, textWidth, AUTO, TITLE_SIZE, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "Search");
   initLabel(&fieldLabel,  &font, 0, 0, textWidth - 2 * FIELD_PAD, AUTO, FIELD_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&recentLabel, &font, MARGIN_X, RECENT_Y, textWidth, AUTO, RECENT_TEXT, activeTheme->textSecondary, TEXT_NOWRAP, "Recent searches");
   initLabel(&emptyLabel,  &font, MARGIN_X, LIST_TOP, textWidth, AUTO, ROW_TEXT, activeTheme->textSecondary, TEXT_NOWRAP, "Nothing yet - your searches will be listed here");
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++)
      initLabel(&rowLabels[i], &font, 0, 0, textWidth - 2 * ROW_PAD, AUTO, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");

   initButtonHints(&hints, &font, state.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),  "Search");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Back");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "Delete");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START),  "Type");

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

static void drawSearchInput(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);

   // the field
   int fieldW = state.screenW - 2 * MARGIN_X;
   int ring = activeTheme->focusThickness;
   if (state.selected == ROW_FIELD)
      fillGfxRectangle(MARGIN_X - ring, FIELD_Y - ring, fieldW + 2 * ring, FIELD_H + 2 * ring, activeTheme->focusBorder);
   fillGfxRectangle(MARGIN_X, FIELD_Y, fieldW, FIELD_H, activeTheme->surface);
   drawLabelAt(&fieldLabel, MARGIN_X + FIELD_PAD, FIELD_Y + (FIELD_H - FIELD_TEXT) / 2 - 2);

   drawLabel(&recentLabel);
   if (state.historyCount == 0) { drawLabel(&emptyLabel); drawButtonHints(&hints, state.screenW); return; }

   for (int row = state.scroll; row < state.historyCount && row < state.scroll + state.visibleRows; row++) {
      int y = LIST_TOP + (row - state.scroll) * ROW_H;
      if (state.selected == row + 1)
         fillGfxRectangle(MARGIN_X - ring, y - ring, fieldW + 2 * ring, ROW_H - 6 + 2 * ring, activeTheme->focusBorder);
      fillGfxRectangle(MARGIN_X, y, fieldW, ROW_H - 6, activeTheme->surface);
      drawLabelAt(&rowLabels[row], MARGIN_X + ROW_PAD, y + (ROW_H - 6 - ROW_TEXT) / 2 - 2);
   }

   drawButtonHints(&hints, state.screenW);
}

static void termSearchInput(void)
{
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&fieldLabel);
   freeLabel(&recentLabel);
   freeLabel(&emptyLabel);
   for (int i = 0; i < MAX_SEARCH_HISTORY; i++) freeLabel(&rowLabels[i]);
   closeFont(&font);
}

void openSearchInput(const char *initialText, SearchInputDone onDone)
{
   strCopy(pendingInitial, sizeof pendingInitial, initialText ? initialText : "");
   doneCallback = onDone;
   pushScreen(&searchInputScreen);
}

Screen searchInputScreen = { initSearchInput, NULL, updateSearchInput, drawSearchInput, NULL, termSearchInput, SCREEN_TERMINATED };

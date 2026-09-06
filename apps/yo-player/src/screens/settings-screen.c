// settings screen - implementation (see settings-screen.h).
//
// Two columns: the options on the left, the app's own card on the right. The card is where the channel link
// lives, so the QR code sits in a place that is always on screen rather than behind a modal nobody opens.

#include "screens/settings-screen.h"
#include "storage.h"
#include "app-paths.h"      // APP_VERSION
#include "sponsorblock.h"        // SponsorCategory + getSponsorCategoryName/Color
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "qr-linktree.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include <string.h>
#include <stdio.h>

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define LIST_TOP      92
#define ROW_H         46
#define ROW_TEXT      24
#define ROW_PAD       14
#define SECTION_TEXT  18
#define SECTION_GAP   34        // height reserved above a row that opens a section
#define VALUE_GAP     30        // right margin of the value column inside the list
#define HINTS_BOTTOM  42
#define HINT_GLYPH_H  25
#define HINT_TEXT     21

#define LIST_WIDTH    860       // wide enough for "Skip by choice" beside the longest category name       // the options column; the card gets the rest
#define CARD_PAD      30
#define CARD_TEXT     24
#define CARD_SMALL    18
#define QR_SCALE      7         // 37 modules incl. quiet zone -> 259 px, comfortably inside the card

// Row order. The two playback options come first because they are the ones changed often; the skip
// categories are a block of their own, which is also the only place "skip the intro" can be turned on.
enum {
   ROW_AUTOPLAY,
   ROW_SKIP_FIRST,
   ROW_COUNT = ROW_SKIP_FIRST + SPONSOR_CATEGORY_COUNT
};

static Font        font;
static Label       titleLabel;
static Label       rowLabels[ROW_COUNT], valueLabels[ROW_COUNT];
static Label       skipSectionLabel, skipHintLabel;
static Label       cardTitleLabel, cardUrlLabel, cardNoteLabel;
static ButtonHints hints;
static ButtonRepeat navigateRepeat;

static struct {
   int screenW, screenH;
   int selected;
} state;

static int skipCategory(int row) { return row - ROW_SKIP_FIRST; }

// the value column for one row, re-read from storage every time so the screen never holds its own copy of
// a setting (which is how a toggle ends up disagreeing with what was actually saved).
// A value says what it is by its COLOUR as much as by its word: active is the accent, inactive is muted.
// Without that the user had to walk the list with the d-pad to find out what was switched on, because
// eight grey words all look alike from a sofa.
static void refreshValue(int row)
{
   const char *text;
   uint32_t colour;
   if (row == ROW_AUTOPLAY) {
      int on = getAutoplay();
      text = on ? "On" : "Off";
      colour = on ? activeTheme->accent : activeTheme->textSecondary;
   } else {
      // Three states, three looks: Skip is the accent, Ask is primary text (it is on, but it waits for
      // you), Keep is muted. The word alone is not enough from a sofa.
      switch (getSponsorAction(skipCategory(row))) {
      case SPONSOR_ACTION_SKIP: text = "Skip";           colour = activeTheme->accent;        break;
      case SPONSOR_ACTION_ASK:  text = "Skip by choice"; colour = activeTheme->textPrimary;   break;
      default:                  text = "Keep";           colour = activeTheme->textSecondary; break;
      }
   }
   setLabelColor(&valueLabels[row], colour);
   setLabelText(&valueLabels[row], text);
}

static void refreshAllValues(void)
{
   for (int row = 0; row < ROW_COUNT; row++) refreshValue(row);
}

// X cycles a skip category Keep -> Skip -> Skip by choice -> Keep. Skip comes first because it is what
// most people want from a sponsor segment; "by choice" is the deliberate extra press.
static void toggleRow(int row)
{
   if (row == ROW_AUTOPLAY) setAutoplay(!getAutoplay());
   else {
      int category = skipCategory(row);
      int next;
      switch (getSponsorAction(category)) {
      case SPONSOR_ACTION_KEEP: next = SPONSOR_ACTION_SKIP; break;
      case SPONSOR_ACTION_SKIP: next = SPONSOR_ACTION_ASK;  break;
      default:                  next = SPONSOR_ACTION_KEEP; break;
      }
      setSponsorAction(category, next);
   }
   refreshValue(row);
}

// y of a row, with the gap that the skip section's heading occupies folded in.
static int rowY(int row)
{
   int y = LIST_TOP + row * ROW_H;
   if (row >= ROW_SKIP_FIRST) y += SECTION_GAP;
   return y;
}

static void initSettings(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.selected = 0;

   font = openSystemFont(FONT_POP);
   int listText = LIST_WIDTH - 2 * ROW_PAD - VALUE_GAP;

   initLabel(&titleLabel, &font, MARGIN_X, HEADER_Y, state.screenW - 2 * MARGIN_X, AUTO, TITLE_SIZE,
             activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "Settings");
   initLabel(&skipSectionLabel, &font, MARGIN_X + ROW_PAD, 0, LIST_WIDTH, AUTO, SECTION_TEXT,
             activeTheme->accent, TEXT_NOWRAP, "SKIP SEGMENTS");
   initLabel(&skipHintLabel, &font, 0, 0, AUTO, AUTO, SECTION_TEXT - 2,
             activeTheme->textSecondary, TEXT_NOWRAP, "from SponsorBlock, also marked on the seek bar");

   for (int row = 0; row < ROW_COUNT; row++) {
      const char *name;
      if (row == ROW_AUTOPLAY) name = "Autoplay next video";
      else                     name = getSponsorCategoryName((SponsorCategory)skipCategory(row));
      initLabel(&rowLabels[row],   &font, 0, 0, listText, AUTO, ROW_TEXT, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, name);
      initLabel(&valueLabels[row], &font, 0, 0, AUTO,     AUTO, ROW_TEXT, activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   refreshAllValues();

   // the card: raw labels, because the URL and the build stamp are literal text, not markup
   initLabelRaw(&cardTitleLabel, &font, 0, 0, AUTO, AUTO, CARD_TEXT,  activeTheme->textPrimary,   TEXT_NOWRAP, "TEE Vanced PS3  " APP_VERSION);
   initLabelRaw(&cardUrlLabel,   &font, 0, 0, AUTO, AUTO, CARD_TEXT,  activeTheme->textPrimary,   TEXT_NOWRAP, QR_URL);
   initLabelRaw(&cardNoteLabel,  &font, 0, 0, AUTO, AUTO, CARD_SMALL, activeTheme->textSecondary, TEXT_NOWRAP, "Scan for links, streams and contact");

   initButtonHints(&hints, &font, state.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, activeTheme->textSecondary);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),  "Change");
   setLabelText(&skipHintLabel, "from SponsorBlock - Skip is automatic, Skip by choice offers a button");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Back");

   memset(&navigateRepeat, 0, sizeof navigateRepeat);
}

static void updateSettings(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }

   if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) state.selected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   state.selected--;
   if (state.selected < 0) state.selected = ROW_COUNT - 1;            // wrap: the list is short enough that
   if (state.selected >= ROW_COUNT) state.selected = 0;               // running off an end should come round

   if (isPadButtonPressed(PAD_BTN_CROSS)) toggleRow(state.selected);
}

static void drawCard(void)
{
   int cardX = MARGIN_X + LIST_WIDTH + 40;
   int cardW = state.screenW - MARGIN_X - cardX;
   int qrSpan = (QR_SIZE + 2 * QR_QUIET) * QR_SCALE;
   int cardH = CARD_PAD + CARD_TEXT + 22 + qrSpan + 20 + CARD_TEXT + 8 + CARD_SMALL + CARD_PAD;
   int cardY = LIST_TOP;

   fillGfxRectangle(cardX - 2, cardY - 2, cardW + 4, cardH + 4, activeTheme->accent);
   fillGfxRectangle(cardX, cardY, cardW, cardH, activeTheme->surface);

   int centre = cardX + cardW / 2, row = cardY + CARD_PAD;
   drawLabelAt(&cardTitleLabel, centre - cardTitleLabel.tt.tex.w / 2, row);   row += CARD_TEXT + 22;
   drawQrCode(centre - qrSpan / 2, row, QR_SCALE);                            row += qrSpan + 20;
   drawLabelAt(&cardUrlLabel,   centre - cardUrlLabel.tt.tex.w / 2, row);     row += CARD_TEXT + 8;
   drawLabelAt(&cardNoteLabel,  centre - cardNoteLabel.tt.tex.w / 2, row);
}

static void drawSettings(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   drawLabel(&titleLabel);

   int sectionY = rowY(ROW_SKIP_FIRST) - SECTION_TEXT - 12;
   moveLabel(&skipSectionLabel, MARGIN_X + ROW_PAD, sectionY);
   drawLabel(&skipSectionLabel);
   drawLabelAt(&skipHintLabel, MARGIN_X + ROW_PAD + skipSectionLabel.tt.tex.w + 16, sectionY + 2);
   fillGfxRectangle(MARGIN_X, sectionY + SECTION_TEXT + 8, LIST_WIDTH, 2, activeTheme->accent);

   for (int row = 0; row < ROW_COUNT; row++) {
      int y = rowY(row), rowH = ROW_H - 6;
      fillGfxRectangle(MARGIN_X, y, LIST_WIDTH, rowH, activeTheme->surface);
      if (row == state.selected) {
         fillGfxRectangle(MARGIN_X, y, LIST_WIDTH, rowH, activeTheme->rowHighlight);
         fillGfxRectangle(MARGIN_X, y, 5, rowH, activeTheme->accent);   // the bar that says "you are here"
      }

      int textY = y + (rowH - ROW_TEXT) / 2 - 2;
      drawLabelAt(&rowLabels[row], MARGIN_X + ROW_PAD + 8, textY);

      // the value is right-aligned in the options column, so the states line up in one readable stripe
      int valueX = MARGIN_X + LIST_WIDTH - ROW_PAD - valueLabels[row].tt.tex.w;
      drawLabelAt(&valueLabels[row], valueX, textY);
   }

   drawCard();
   drawButtonHints(&hints, state.screenW);
}

static void termSettings(void)
{
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&skipSectionLabel);
   freeLabel(&skipHintLabel);
   for (int row = 0; row < ROW_COUNT; row++) { freeLabel(&rowLabels[row]); freeLabel(&valueLabels[row]); }
   freeLabel(&cardTitleLabel);
   freeLabel(&cardUrlLabel);
   freeLabel(&cardNoteLabel);
   closeFont(&font);
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen settingsScreen = { initSettings, NULL, updateSettings, drawSettings, NULL, termSettings, SCREEN_TERMINATED };

void openSettingsScreen(void) { pushScreen(&settingsScreen); }

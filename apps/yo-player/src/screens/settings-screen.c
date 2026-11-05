// settings screen - implementation (see settings-screen.h).
//
// Two columns: the options on the left, the app's own card on the right. The card is where the channel link
// lives, so the QR code sits in a place that is always on screen rather than behind a modal nobody opens.

#include "screens/settings-screen.h"
#include "storage.h"
#include "app-paths.h"      // APP_VERSION
#include "sponsorblock.h"        // SponsorCategory + getSponsorCategoryName/Color
#include "ui-hints.h"            // UiHints; brings ui-draw.h (UI_SAFE_*) and ui-fonts.h (uiInitLabel, metrics)
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

// Everything sits inside the safe area (UI_SAFE_X .. UI_SAFE_RIGHT, ui-draw.h) of the 1920x1080 canvas.
#define TITLE_BASELINE   107    // the page title's baseline, the same as on the list pages (Bild 4)
#define LIST_TOP         160
// A row plate is ROW_H - ROW_GAP = 54 px: 24-px text centred on its capitals keeps its descenders
// ("Self-promotion", "Autoplay") well inside the plate.
#define ROW_H            62     // row pitch
#define ROW_GAP          8
#define ROW_TEXT         24
#define ROW_WEIGHT       UI_WEIGHT_BOLD
#define ROW_PAD          14
#define SECTION_GAP      56     // height reserved above a row that opens a section
#define SECTION_BASE_UP  22     // the section heading's baseline, this far above the section's first row
#define SECTION_RULE_UP  10     // the rule under the heading, this far above the same row
#define SECTION_TEXT_GAP 16     // heading -> its explanation, on the same line
#define VALUE_GAP        30     // right margin of the value column inside the list
#define VALUE_COLUMN     200    // kept free for the widest value, "Skip by choice" (about 170 px at 24/700)

#define LIST_WIDTH    900       // the options column: wide enough for the section heading and its explanation
                                // on one line, and for "Skip by choice" beside the longest category name
                                // ("Interaction reminder"); the card gets the rest
#define CARD_GAP       40       // options column -> card
#define CARD_PAD       30
#define CARD_TEXT      24
#define CARD_TITLE_GAP 24       // the title's baseline -> the QR code
#define CARD_QR_GAP    20       // the QR code -> the link's capitals
#define CARD_LINE_GAP  16       // the link's baseline -> the note's capitals
#define QR_SCALE      7         // 37 modules incl. quiet zone -> 259 px, comfortably inside the card

// Row order. The two playback options come first because they are the ones changed often; the skip
// categories are a block of their own, which is also the only place "skip the intro" can be turned on.
enum {
   ROW_AUTOPLAY,
   ROW_SKIP_FIRST,
   ROW_COUNT = ROW_SKIP_FIRST + SPONSOR_CATEGORY_COUNT
};

static Label       titleLabel;
static Label       rowLabels[ROW_COUNT], valueLabels[ROW_COUNT];
static Label       skipSectionLabel, skipHintLabel;
static Label       cardTitleLabel, cardUrlLabel, cardNoteLabel;
static UiHints     hints;
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

// From the top of a label's capitals to its baseline, measured for its own font and size (ui-fonts.h): the
// card is laid out by these, because a texture's first rows are blank by an amount that depends on the font.
static int capHeightOf(const Label *l)
{
   UiTextMetrics m = uiGetTextMetrics(l->font, l->size);
   return m.baseline - m.capTop;
}

static void initSettings(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.selected = 0;

   // Fonts are shared app-wide (ui-fonts.h): nothing is opened here and nothing may be closed in term.
   // All of this screen's text is written in the code, so it is Manrope throughout.
   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, UI_SAFE_X, 0, UI_SAFE_RIGHT - UI_SAFE_X, UI_PAGE_TITLE_SIZE,
               activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "Settings");
   // an eyebrow heading the way the Ember start design draws one ("CONTINUE WATCHING"): capitals at 20/800
   // in accent-text - the accent itself is kept for focus, progress and the chosen value
   uiInitLabel(&skipSectionLabel, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, UI_META_SIZE, activeTheme->accentText,
               TEXT_NOWRAP, "SKIP SEGMENTS");
   // Beside the heading, capped at what is left of the options column: at 20 px it fits there with some 60 px
   // to spare (LIST_WIDTH), and should a font ever draw it wider it ends in "..." rather than in the card.
   int explainW = LIST_WIDTH - ROW_PAD - skipSectionLabel.tt.tex.w - SECTION_TEXT_GAP;
   uiInitLabel(&skipHintLabel, UI_META_WEIGHT, 0, 0, explainW, UI_META_SIZE, activeTheme->textSecondary,
               TEXT_NOWRAP_ELLIPSIS, "from SponsorBlock - Skip is automatic, Skip by choice offers a button");

   // the names end where the widest value could begin, so even "Interaction reminder" never meets it
   int nameW = LIST_WIDTH - 2 * ROW_PAD - 8 - VALUE_GAP - VALUE_COLUMN;
   for (int row = 0; row < ROW_COUNT; row++) {
      const char *name;
      if (row == ROW_AUTOPLAY) name = "Autoplay next video";
      else                     name = getSponsorCategoryName((SponsorCategory)skipCategory(row));
      uiInitLabel(&rowLabels[row],   ROW_WEIGHT, 0, 0, nameW, ROW_TEXT, activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, name);
      uiInitLabel(&valueLabels[row], ROW_WEIGHT, 0, 0, AUTO,  ROW_TEXT, activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   refreshAllValues();

   // the card: raw labels, because the URL and the build stamp are literal text, not markup (both are ASCII,
   // so uiInitTextLabel keeps them in Manrope)
   uiInitTextLabel(&cardTitleLabel, UI_WEIGHT_BOLD, 0, 0, AUTO, CARD_TEXT,    activeTheme->textPrimary,   TEXT_NOWRAP, "TEE Vanced PS3  " APP_VERSION);
   uiInitTextLabel(&cardUrlLabel,   UI_WEIGHT_BOLD, 0, 0, AUTO, CARD_TEXT,    activeTheme->textPrimary,   TEXT_NOWRAP, QR_URL);
   uiInitTextLabel(&cardNoteLabel,  UI_META_WEIGHT, 0, 0, AUTO, UI_META_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "Scan for links, streams and contact");

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "Change");
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");

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
   int cardX = UI_SAFE_X + LIST_WIDTH + CARD_GAP;
   int cardW = UI_SAFE_RIGHT - cardX;
   int qrSpan = (QR_SIZE + 2 * QR_QUIET) * QR_SCALE;
   int cardY = LIST_TOP;

   // laid out from the top by baselines: the title's capitals start CARD_PAD in, the code hangs under the
   // title, the link and the note follow the code, and CARD_PAD is left under the note's baseline
   int titleBase = cardY + CARD_PAD + capHeightOf(&cardTitleLabel);
   int qrY       = titleBase + CARD_TITLE_GAP;
   int urlBase   = qrY + qrSpan + CARD_QR_GAP + capHeightOf(&cardUrlLabel);
   int noteBase  = urlBase + CARD_LINE_GAP + capHeightOf(&cardNoteLabel);
   int cardH     = noteBase + CARD_PAD - cardY;

   // An Ember panel: surface-1 with a hairline edge drawn inward, so the card ends on the safe area's edge.
   // The edge used to be the accent, but an orange outline is what focus looks like now, and nothing on
   // this card can be focused.
   fillGfxRectangle(cardX, cardY, cardW, cardH, activeTheme->surface1);
   strokeGfxRectangle(cardX, cardY, cardW, cardH, 2, activeTheme->hairline);

   int centre = cardX + cardW / 2;
   drawLabelAt(&cardTitleLabel, centre - cardTitleLabel.tt.tex.w / 2, uiLabelYForBaseline(&cardTitleLabel, titleBase));
   drawQrCode(centre - qrSpan / 2, qrY, QR_SCALE);
   drawLabelAt(&cardUrlLabel,   centre - cardUrlLabel.tt.tex.w / 2,   uiLabelYForBaseline(&cardUrlLabel, urlBase));
   drawLabelAt(&cardNoteLabel,  centre - cardNoteLabel.tt.tex.w / 2,  uiLabelYForBaseline(&cardNoteLabel, noteBase));
}

static void drawSettings(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   uiPlaceLabelBaseline(&titleLabel, UI_SAFE_X, TITLE_BASELINE);
   drawLabel(&titleLabel);

   // the section heading and its explanation share one baseline; the rule runs under both
   int firstRow = rowY(ROW_SKIP_FIRST);
   int sectionBase = firstRow - SECTION_BASE_UP;
   uiPlaceLabelBaseline(&skipSectionLabel, UI_SAFE_X + ROW_PAD, sectionBase);
   drawLabel(&skipSectionLabel);
   uiPlaceLabelBaseline(&skipHintLabel, UI_SAFE_X + ROW_PAD + skipSectionLabel.tt.tex.w + SECTION_TEXT_GAP,
                        sectionBase);
   drawLabel(&skipHintLabel);
   fillGfxRectangle(UI_SAFE_X, firstRow - SECTION_RULE_UP, LIST_WIDTH, 2, activeTheme->hairline);

   for (int row = 0; row < ROW_COUNT; row++) {
      int y = rowY(row), rowH = ROW_H - ROW_GAP;
      fillGfxRectangle(UI_SAFE_X, y, LIST_WIDTH, rowH, activeTheme->surface);
      if (row == state.selected) {
         fillGfxRectangle(UI_SAFE_X, y, LIST_WIDTH, rowH, activeTheme->rowHighlight);
         fillGfxRectangle(UI_SAFE_X, y, 5, rowH, activeTheme->accent);   // the bar that says "you are here"
      }

      // name and value are the same font and size, so one y centres the capitals of both in the row
      int textY = uiLabelYForCenter(&rowLabels[row], y, rowH);
      drawLabelAt(&rowLabels[row], UI_SAFE_X + ROW_PAD + 8, textY);

      // the value is right-aligned in the options column, so the states line up in one readable stripe
      int valueX = UI_SAFE_X + LIST_WIDTH - ROW_PAD - valueLabels[row].tt.tex.w;
      drawLabelAt(&valueLabels[row], valueX, textY);
   }

   drawCard();
   // bottom left, under the options, where the Ember settings design (Bild 3) puts this screen's hints
   uiDrawHints(&hints, UI_SAFE_X, UI_HINTS_BOTTOM_Y, UI_ALIGN_LEFT);
}

static void termSettings(void)
{
   uiTermHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&skipSectionLabel);
   freeLabel(&skipHintLabel);
   for (int row = 0; row < ROW_COUNT; row++) { freeLabel(&rowLabels[row]); freeLabel(&valueLabels[row]); }
   freeLabel(&cardTitleLabel);
   freeLabel(&cardUrlLabel);
   freeLabel(&cardNoteLabel);
   // no closeFont: the fonts belong to ui-fonts.c and every other screen is still drawing with them
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen settingsScreen = { initSettings, NULL, updateSettings, drawSettings, NULL, termSettings, SCREEN_TERMINATED };

void openSettingsScreen(void) { pushScreen(&settingsScreen); }

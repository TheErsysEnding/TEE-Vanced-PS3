// language-screen (V3.1.0) - the language picker (see language-screen.h), as TEE Witch has it.
//
// Japanese, Korean and Chinese are drawn in the console's FONT_POP: a language whose texts it has no glyph for -
// Chinese where the console's font set lacks simplified characters - is left out of the list (the log says which),
// rather than offered with a menu of blanks. Arabic is drawn in DejaVu Sans Bold, right to left within each line.

#include "screens/language-screen.h"
#include "i18n.h"
#include "screen-manager.h"
#include "gfx.h"
#include "ui-fonts.h"
#include "ui-draw.h"
#include "ui-hints.h"
#include "ui/console-glyphs.h"
#include "theme.h"
#include "pad.h"
#include "button-repeat.h"
#include "dbg.h"

#include <string.h>

#define MAX_LANGS 32
#define COLS      4
#define CARD_W    400
#define CARD_H    88
#define GAP_X     22
#define GAP_Y     16
#define GRID_X    ((UI_SCREEN_W - (COLS * CARD_W + (COLS - 1) * GAP_X)) / 2)
#define GRID_Y    272
#define FLAG_W    72
#define FLAG_H    48
#define LOGO_SIZE 64      // the rail's logo tile: "TEE" on the accent
#define LOGO_GAP  32

static struct {
   int        firstStart;
   int        langs[MAX_LANGS], count;   // the i18n indexes shown
   int        sel;                       // into langs
   Label      names[MAX_LANGS];
   GfxTexture flags[MAX_LANGS];
   Label      title, subtitle, logo;
   int        textsFor;                  // the language title, subtitle and hints are in (-1 none)
   UiHints    hints;
   ButtonRepeat repeatH, repeatV;
} st;

// whether the console's fonts draw every text of a language (only a CJK one can fall short)
static int drawable(int lang)
{
   if (i18nScriptOf(lang) != I18N_CJK) return 1;
   int missing = i18nCountMissing(lang, uiFallbackMissing);
   if (missing) logWarn("[language] %s: %d characters the console's font does not have - not offered\n", i18nCode(lang), missing);
   return missing == 0;
}

static void freeTexts(void)
{
   if (st.textsFor < 0) return;
   freeLabel(&st.title);
   freeLabel(&st.subtitle);
   uiTermHints(&st.hints);
   st.textsFor = -1;
}

// the title, the line under it and the hints in the language under the cursor
static void makeTexts(void)
{
   int lang = st.langs[st.sel];
   if (st.textsFor == lang) return;
   freeTexts();
   int textW = UI_SAFE_RIGHT - (GRID_X + LOGO_SIZE + LOGO_GAP);
   uiInitLabel(&st.title, UI_PAGE_TITLE_WEIGHT, 0, 0, textW, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP_ELLIPSIS, trIn(lang, "Choose your language"));
   uiInitLabel(&st.subtitle, UI_META_WEIGHT, 0, 0, textW, 24, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS,
               st.firstStart ? trIn(lang, "You can change it later in Settings.")
                             : trIn(lang, "The app's own texts. Titles and names from YouTube stay as they are."));
   uiInitHints(&st.hints);
   uiAddHint(&st.hints, GLYPH_CROSS, trIn(lang, "Choose"));
   if (!st.firstStart) uiAddHint(&st.hints, GLYPH_CIRCLE, trIn(lang, "Back"));
   st.textsFor = lang;
}

static void initLanguage(void)
{
   st.textsFor = -1;
   st.count = 0;
   for (int i = 0; i < i18nCount() && st.count < MAX_LANGS; i++)
      if (i == i18nIndex() || drawable(i)) st.langs[st.count++] = i;
   for (int k = 0; k < st.count; k++) {
      int lang = st.langs[k];
      uiInitLabel(&st.names[k], UI_WEIGHT_BOLD, 0, 0, CARD_W - FLAG_W - 64, 26, activeTheme->textPrimary,
                  TEXT_NOWRAP_ELLIPSIS, i18nName(lang));
      unsigned int size = 0;
      const uint8_t *png = i18nFlag(lang, &size);
      if (png) st.flags[k] = loadGfxTextureMem(png, size);
      if (lang == i18nIndex()) st.sel = k;
   }
   uiInitTextLabel(&st.logo, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, 21, activeTheme->onAccent, TEXT_NOWRAP, "TEE");
   memset(&st.repeatH, 0, sizeof st.repeatH);
   memset(&st.repeatV, 0, sizeof st.repeatV);
   makeTexts();
   logInfo("[language] picker%s: %d languages\n", st.firstStart ? " (first start)" : "", st.count);
}

static void termLanguage(void)
{
   freeTexts();
   freeLabel(&st.logo);
   for (int k = 0; k < st.count; k++) {
      freeLabel(&st.names[k]);
      if (st.flags[k].offset) retireGfxTexture(&st.flags[k]);
   }
}

static void updateLanguage(void)
{
   int col = st.sel % COLS, row = st.sel / COLS;
   if (isRepeatDue(&st.repeatH, getPadButtonState(PAD_BTN_RIGHT)) && st.sel + 1 < st.count && col + 1 < COLS) st.sel++;
   if (isRepeatDue(&st.repeatH, getPadButtonState(PAD_BTN_LEFT)) && col > 0) st.sel--;
   if (isRepeatDue(&st.repeatV, getPadButtonState(PAD_BTN_DOWN))) {
      if (st.sel + COLS < st.count) st.sel += COLS;
      else if ((row + 1) * COLS < st.count) st.sel = st.count - 1;   // the last row is short: its last one
   }
   if (isRepeatDue(&st.repeatV, getPadButtonState(PAD_BTN_UP)) && row > 0) st.sel -= COLS;
   makeTexts();
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      setI18nIndex(st.langs[st.sel]);   // in use at once; the screens below make their labels anew (i18nRevision)
      popScreen();
      return;
   }
   if (!st.firstStart && isPadButtonPressed(PAD_BTN_CIRCLE)) popScreen();
}

static void drawLanguage(void)
{
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->appBg);
   UiGradientStop glow[] = { { 0.0f, uiWithAlpha(activeTheme->accent, 0x22) }, { 1.0f, uiWithAlpha(activeTheme->accent, 0) } };
   uiFillGradientV(0, 0, UI_SCREEN_W, 260, glow, 2);
   int logoY = 96;
   uiFillRounded(GRID_X, logoY, LOGO_SIZE, LOGO_SIZE, 18, activeTheme->accent);
   uiDrawLabelAt(&st.logo, GRID_X + (LOGO_SIZE - st.logo.tt.tex.w) / 2, uiLabelYForCenter(&st.logo, logoY, LOGO_SIZE));
   int textX = GRID_X + LOGO_SIZE + LOGO_GAP;
   uiDrawLabelAt(&st.title, textX, uiLabelYForBaseline(&st.title, logoY + 40));
   uiDrawLabelAt(&st.subtitle, textX, uiLabelYForBaseline(&st.subtitle, logoY + 86));
   for (int k = 0; k < st.count; k++) {
      int x = GRID_X + (k % COLS) * (CARD_W + GAP_X);
      int y = GRID_Y + (k / COLS) * (CARD_H + GAP_Y);
      int sel = k == st.sel, current = st.langs[k] == i18nIndex() && i18nWasChosen();
      if (sel) uiDrawFocusGlow(x, y, CARD_W, CARD_H, activeTheme->focusGlow);
      uiFillRounded(x, y, CARD_W, CARD_H, UI_RADIUS_ROW, sel ? activeTheme->surface3 : activeTheme->surface1);
      int fx = x + 22, fy = y + (CARD_H - FLAG_H) / 2;
      uiFillRect(fx - 2, fy - 2, FLAG_W + 4, FLAG_H + 4, activeTheme->surface3);   // a frame: white flags on light cards
      if (st.flags[k].offset)
         drawGfxTexture(fx, fy, FLAG_W, FLAG_H, st.flags[k], 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, GFX_FILTER_LINEAR);
      Label *l = &st.names[k];
      uiDrawLabelAt(l, fx + FLAG_W + 22, uiLabelYForCenter(l, y, CARD_H));
      if (current) uiFillCircleAt(x + CARD_W - 20, y + CARD_H / 2, 12, activeTheme->accent);
      if (sel) uiDrawFocusRing(x, y, CARD_W, CARD_H, UI_RING_SEGMENT, activeTheme->focusBorder);
   }
   uiDrawHints(&st.hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
}

static void resumeLanguage(void) {}
static void suspendLanguage(void) {}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen languageScreen = { initLanguage, resumeLanguage, updateLanguage, drawLanguage, suspendLanguage, termLanguage,
                                 SCREEN_TERMINATED };

void openLanguagePicker(int firstStart)
{
   memset(&st, 0, sizeof st);
   st.firstStart = firstStart;
   pushScreen(&languageScreen);
}

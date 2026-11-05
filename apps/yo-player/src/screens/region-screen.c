// region screen - implementation (see region-screen.h). The rows are the playlist chooser's, which are the
// Ember settings rows (Bild 3): no plate of their own, the focused one on a rounded plate with the focus ring
// and its glow. The list is longer than a screen, so a window of rows follows the selection, and the header
// says where in the list you are, as Home's page counter does.

#include "screens/region-screen.h"
#include "settings.h"           // the country table and the region in use
#include "sidebar.h"            // drawSidebarPassive: the rail marks Settings, the page this screen belongs to
#include "ui-clock.h"           // the time of day at the top right, where Settings has it
#include "ui-hints.h"           // UiHints; brings ui-draw.h (UI_SAFE_*, UiDigits) and ui-fonts.h (labels, metrics)
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
#include "dbg.h"                // logWarn
#include <sys/sys_time.h>       // sys_time_get_system_time: the repeat timers
#include <string.h>
#include <stdio.h>

// Everything sits inside the safe area, beside the collapsed rail: the content starts at x 176 as on every
// Ember page. The title's capitals are centred on the band the list pages' header uses (home.c), where the
// settings screen underneath has its own title, so the heading does not jump when this screen opens.
#define CONTENT_X      176
#define HEADER_Y       67
#define HEADER_H       45
#define HEADER_GAP     40       // the position readout -> the clock, as Home's page counter keeps it
#define CLOCK_SIZE     28       // ui-clock.h's clock is 28/700: needed to centre its capitals on the band
#define CLOCK_WEIGHT   UI_WEIGHT_BOLD
#define POS_SIZE       24       // "7 / 33": 24/700 in text-2 with equal-width digits, like Home's page counter
#define POS_WEIGHT     UI_WEIGHT_BOLD
#define NOTE_DY        43       // the title's baseline -> the note's: where Settings has its version line
#define NOTE_LIST_GAP  42       // the note's last baseline -> the first plate: y 192, Settings' body line
// The rows are the playlist chooser's: a 62-px pitch, a 54-px plate, 24/700 names, 1152 px wide.
#define ROW_H          62
#define PLATE_H        (ROW_H - 8)
#define LIST_W         1152
#define ROW_TEXT       24
#define ROW_WEIGHT     UI_WEIGHT_BOLD
#define ROW_PAD        20       // plate edge -> the check, and the country code -> plate edge
#define ICON_SIZE      26
#define ICON_GAP       14       // the check -> the name; every name keeps the check's room, so they line up
#define CODE_W         60       // kept for the code ("DE" is about 30 px at 20/600)
#define CODE_GAP       30
#define LIST_CLEAR     30       // the last row's plate ends at least this far above the hint row (its ring 21)
#define MAX_ROWS       40       // the table (settings.c) has about thirty; a longer one is cut here, with a warning
// The names are rendered once in white and tinted when drawn: the country in use is in the accent's text
// colour, every other one in text-1, and re-colouring a rendered label means rasterising it again.
#define TEXT_TINTABLE  0xFFFFFFFFu
#define NOTE_TEXT      "Trending and Live come from this country's YouTube; Search and Sports change only partly. " \
                       "The app stays in English."

static Label        titleLabel, noteLabel;
static Label        nameLabels[MAX_ROWS], codeLabels[MAX_ROWS];
static Icon         checkIcon;
static UiDigits     positionDigits;
static UiHints      hints;
static ButtonRepeat navigateRepeat, pageRepeat;

static struct {
   int screenW, screenH;
   int count;              // rows: the table's countries, at most MAX_ROWS
   int current;            // the country in use, marked with the check
   int selected;
   int top;                // the first row the window shows
   int visible;            // how many rows the window shows
   int titleY, noteBaseline, clockBaseline, listTop;
   uint32_t plateFill;     // the focused plate: hintFill over the page, opaque (as the playlist chooser's)
} state;

// The baseline of a label's last line, given its first: every line after the first adds one pitch to the
// texture, so the texture's height says how many there are.
static int lastBaselineOf(const Label *l, int firstBaseline)
{
   UiTextMetrics m = uiGetTextMetrics(l->font, l->size);
   int extra = l->tt.tex.h - m.texH;
   return firstBaseline + (extra > 0 ? extra : 0);
}

// ---- the window of rows ----

static void clampTop(void)
{
   int maxTop = state.count - state.visible;
   if (state.top > maxTop) state.top = maxTop;
   if (state.top < 0) state.top = 0;
}

// moves the window as little as it takes to keep the selection on screen
static void followSelection(void)
{
   if (state.selected < state.top) state.top = state.selected;
   if (state.selected >= state.top + state.visible) state.top = state.selected - state.visible + 1;
   clampTop();
}

// UP/DOWN: one row, wrapping at the ends like every list in the app; the window follows, so a wrap from the
// last country to the first shows the top of the list again
static void stepSelection(int delta)
{
   state.selected += delta;
   if (state.selected < 0) state.selected = state.count - 1;
   if (state.selected >= state.count) state.selected = 0;
   followSelection();
}

// L2/R2: a screenful, as they turn the page on every list - the window and the selection move together, so the
// selection keeps its place on screen - stopping at the first and the last country rather than wrapping
static void jumpPage(int delta)
{
   int target = state.selected + delta * state.visible;
   if (target < 0) target = 0;
   if (target > state.count - 1) target = state.count - 1;
   state.selected = target;
   state.top += delta * state.visible;
   clampTop();
   followSelection();
}

// X: the country under the selection is in use from the next request, and saved (settings.c logs both, and
// whether the save worked - a failed one still changes the region for this session, as setAutoplay does).
// Picking the country already in use writes nothing, unless its last save failed: then it is tried again.
static void pickSelected(void)
{
   setContentRegionIndex(state.selected);
   popScreen();
}

static void initRegionScreen(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.plateFill = interpolateColor(activeTheme->appBg, uiWithAlpha(activeTheme->hintFill, 0xFF),
                                      (float)(activeTheme->hintFill >> 24) / 255.0f);

   state.count = getContentRegionCount();
   if (state.count > MAX_ROWS) {
      logWarn("[region] %d countries, the picker shows the first %d\n", state.count, MAX_ROWS);
      state.count = MAX_ROWS;
   }
   if (state.count < 0) state.count = 0;
   state.current = getContentRegionIndex();
   state.selected = (state.current >= 0 && state.current < state.count) ? state.current : 0;

   // Fonts are shared app-wide (ui-fonts.h): nothing is opened here and nothing may be closed in term. Every
   // text here is the app's own - the country names are English and ASCII - so it is Manrope throughout.
   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Region");
   // one line (about 1180 px of the 1648 there are); wrapped rather than cut should a font ever draw it
   // wider, and the list starts under its last line
   uiInitLabel(&noteLabel, UI_BODY_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_BODY_SIZE,
               activeTheme->textSecondary, TEXT_WRAP, NOTE_TEXT);

   int nameW = LIST_W - 2 * ROW_PAD - ICON_SIZE - ICON_GAP - CODE_GAP - CODE_W;
   for (int i = 0; i < state.count; i++) {
      const ContentRegion *region = getContentRegionAt(i);
      uiInitLabel(&nameLabels[i], ROW_WEIGHT, 0, 0, nameW, ROW_TEXT, TEXT_TINTABLE, TEXT_NOWRAP_ELLIPSIS,
                  region ? region->name : "");
      // the code as settings.txt has it (region=DE): what the log names too
      uiInitLabel(&codeLabels[i], UI_META_WEIGHT, 0, 0, AUTO, UI_META_SIZE, activeTheme->textSecondary,
                  TEXT_NOWRAP, region ? region->code : "");
   }
   initIcon(&checkIcon, ICON_OK, ICON_SIZE);
   uiInitDigits(&positionDigits, POS_WEIGHT, POS_SIZE);

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,     "Choose");
   uiAddHint(&hints, GLYPH_CIRCLE,    "Back");
   uiAddHint(&hints, GLYPH_DPAD_UP,   "");
   uiAddHint(&hints, GLYPH_DPAD_DOWN, "Move");
   uiAddHint(&hints, GLYPH_L2,        "");
   uiAddHint(&hints, GLYPH_R2,        "Page");

   // The layout hangs off the title, as Settings' does; the note's measured height places the list.
   state.titleY       = uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H);
   state.noteBaseline = state.titleY + uiGetTextMetrics(titleLabel.font, titleLabel.size).baseline + NOTE_DY;
   state.listTop      = lastBaselineOf(&noteLabel, state.noteBaseline) + NOTE_LIST_GAP;
   Font *clockFont     = uiGetFont(CLOCK_WEIGHT);
   state.clockBaseline = uiTextYForCenter(clockFont, CLOCK_SIZE, HEADER_Y, HEADER_H)
                         + uiGetTextMetrics(clockFont, CLOCK_SIZE).baseline;
   // as many whole rows as fit above the hint row: 12 with a one-line note
   state.visible = (UI_HINTS_BOTTOM_Y - LIST_CLEAR - state.listTop - PLATE_H) / ROW_H + 1;
   if (state.visible > state.count) state.visible = state.count;
   if (state.visible < 1) state.visible = 1;
   // the country in use in the middle of the window, as far as the ends of the list allow
   state.top = state.selected - state.visible / 2;
   clampTop();

   // A direction or L2/R2 still held from Settings must wait the normal hold delay rather than fire on the
   // first frame: a zeroed timer reads as "last fired an eternity ago" (video-grid.c, resetGridRepeat).
   uint64_t now = sys_time_get_system_time();
   navigateRepeat.timer = now; navigateRepeat.repeats = 0;
   pageRepeat.timer     = now; pageRepeat.repeats     = 0;

   uiLogVram("region picker");
}

static void updateRegionScreen(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }   // nothing changes
   if (state.count <= 0) return;

   if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) stepSelection(1);
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   stepSelection(-1);
   if      (isRepeatDue(&pageRepeat, getPadButtonState(PAD_BTN_R2))) jumpPage(1);
   else if (isRepeatDue(&pageRepeat, getPadButtonState(PAD_BTN_L2))) jumpPage(-1);

   if (isPadButtonPressed(PAD_BTN_CROSS)) pickSelected();
}

// a label rendered in TEXT_TINTABLE, drawn at (x, y) in `color` (an opaque theme colour)
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

static int rowTop(int row) { return state.listTop + (row - state.top) * ROW_H; }

// The baseline that centres row text in a plate starting at plateY, measured on Manrope: the name and the
// code beside it both sit on it.
static int rowBaseline(int plateY)
{
   Font *font = uiGetFont(ROW_WEIGHT);
   return uiTextYForCenter(font, ROW_TEXT, plateY, PLATE_H) + uiGetTextMetrics(font, ROW_TEXT).baseline;
}

// The focused row's plate, as the playlist chooser draws it: the glow first, so it shows round the plate and
// not over it, then the plate and the ring.
static void drawFocusPlate(int y)
{
   uiDrawFocusGlow(CONTENT_X, y, LIST_W, PLATE_H, activeTheme->focusGlow);
   uiFillRounded(CONTENT_X, y, LIST_W, PLATE_H, UI_RADIUS_ROW, state.plateFill);
   uiDrawFocusRing(CONTENT_X, y, LIST_W, PLATE_H, UI_RING_CARD, activeTheme->focusBorder);
}

// A row: the check if this is the country in use, its name - in the accent's text colour when it is in use -
// and its code at the right end, where the playlist rows have their count.
static void drawRow(int row, int y)
{
   int inUse = (row == state.current);
   int baseline = rowBaseline(y);
   if (inUse && checkIcon.tex)   // an icon with its ink centred on the plate (drawIcon places the em box)
      drawIcon(&checkIcon, CONTENT_X + ROW_PAD, y + (PLATE_H - checkIcon.tex->h) / 2 - checkIcon.offsetY,
               activeTheme->accent);

   const Label *name = &nameLabels[row];
   drawTinted(name, CONTENT_X + ROW_PAD + ICON_SIZE + ICON_GAP, uiLabelYForBaseline(name, baseline),
              inUse ? activeTheme->accentText : activeTheme->textPrimary);
   const Label *code = &codeLabels[row];
   uiDrawLabelAt(code, CONTENT_X + LIST_W - ROW_PAD - code->tt.tex.w, uiLabelYForBaseline(code, baseline));
}

// "7 / 33" on the header band, left of the clock where Home has its page counter: the list runs past the
// screen, and this says how far.
static void drawPosition(void)
{
   char text[24];
   snprintf(text, sizeof text, "%d / %d", state.selected + 1, state.count);
   int right = UI_SAFE_RIGHT - uiGetClockWidth() - HEADER_GAP;
   int y = uiTextYForCenter(positionDigits.font, positionDigits.size, HEADER_Y, HEADER_H);
   uiDrawDigits(&positionDigits, right - uiGetDigitsWidth(&positionDigits, text), y, text, activeTheme->textSecondary);
}

static void drawRegionScreen(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   drawSidebarPassive(state.screenH, SIDE_SETTINGS);

   int end = state.top + state.visible;
   if (end > state.count) end = state.count;
   if (state.selected >= state.top && state.selected < end) drawFocusPlate(rowTop(state.selected));
   for (int row = state.top; row < end; row++) drawRow(row, rowTop(row));

   // the header after the list: the first row's glow reaches up under the note and the title, and text drawn
   // before the glow would come out tinted orange
   uiDrawLabelAt(&titleLabel, CONTENT_X, state.titleY);
   uiDrawLabelAt(&noteLabel, CONTENT_X, uiLabelYForBaseline(&noteLabel, state.noteBaseline));
   if (state.count > 0) drawPosition();
   uiDrawClock(UI_SAFE_RIGHT, state.clockBaseline);

   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
}

static void termRegionScreen(void)
{
   uiTermHints(&hints);
   freeIcon(&checkIcon);
   uiFreeDigits(&positionDigits);
   freeLabel(&titleLabel);
   freeLabel(&noteLabel);
   for (int i = 0; i < MAX_ROWS; i++) { freeLabel(&nameLabels[i]); freeLabel(&codeLabels[i]); }
   // no closeFont: the fonts belong to ui-fonts.c and every other screen is still drawing with them
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen regionScreen = { initRegionScreen, NULL, updateRegionScreen, drawRegionScreen, NULL,
                               termRegionScreen, SCREEN_TERMINATED };

void openRegionScreen(void) { pushScreen(&regionScreen); }

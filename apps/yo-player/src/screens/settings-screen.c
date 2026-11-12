// settings screen - implementation (see settings-screen.h).
//
// The Ember settings page (Bild 3) with only what the app has. The collapsed rail sits at the left edge,
// drawn passive: Circle is Back on this pushed screen, so the rail only says where you are. Beside it the
// title with the version under it, the clock on the right. Below, the About card stands in the column where
// Bild 3 has its category list, and one panel holds every option, Playback above SponsorBlock. It is still
// ONE list - UP/DOWN walk all nine rows and wrap, as they always did - so no section needs a way of its own
// to be reached. The card is where the channel link lives, so the QR code sits in a place that is always on
// screen rather than behind a modal nobody opens. The one row that is not a toggle, Region, opens its picker
// (region-screen.h) on X.

#include "screens/settings-screen.h"
#include "screens/region-screen.h"   // openRegionScreen: X on the Region row
#include "storage.h"
#include "settings.h"            // the content region: its name for the Region row
#include "app-paths.h"      // APP_VERSION
#include "update-check.h"        // (V3.0.0 Test 12) a newer release, under the version
#include "sponsorblock.h"        // SponsorCategory + getSponsorCategoryName/Color
#include "sidebar.h"             // the rail: collapsed, or open over the screen while it has the focus
#include "screens/home.h"        // updateRailOverScreen: LEFT opens the rail here as on Home
#include "ui-clock.h"            // the time of day at the top right, as on the list pages
#include "ui-hints.h"            // UiHints; brings ui-draw.h (UI_SAFE_*) and ui-fonts.h (uiInitLabel, metrics)
#include "ui/console-glyphs.h"
#include "qr-linktree.h"

#include "gfx.h"
#include "theme.h"
#include "anim.h"                // interpolateColor: the panels' hairline edge, blended once
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include <string.h>
#include <stdio.h>

// Everything sits inside the safe area of the 1920x1080 canvas, beside the collapsed rail (112 px wide):
// content runs from x 176 to UI_SAFE_RIGHT, as on every Ember page.
#define CONTENT_X        176
// The title and the clock have their capitals centred on the band the list pages' header uses (home.c), so
// neither moves when this page opens over Home: the title's baseline comes to 107 as in Bild 4. Bild 3 hangs
// its clock lower, level with the version line; one place for it on every page reads calmer.
#define HEADER_Y         67
#define HEADER_H         45
#define CLOCK_SIZE       28     // ui-clock.h's clock is 28/700: needed to centre its capitals on the band
#define CLOCK_WEIGHT     UI_WEIGHT_BOLD
#define VERSION_DY       43     // the title's baseline -> the version line's: "TEE Vanced V1.0", 20/600
                                // in text-3, as in Bild 3
#define BODY_Y           192    // the About card and the options panel start side by side here
#define HEADING_SIZE     34     // the card's and the sections' titles: 34/800, Bild 3's panel title
#define HEADING_WEIGHT   UI_WEIGHT_EXTRABOLD
#define HEADING_DY       57     // BODY_Y -> those titles' baseline: their capitals 32 px into the box
#define FIRST_ROW_DY     85     // BODY_Y -> the Autoplay row, and the QR code level with it
#define BOX_RADIUS       UI_RADIUS_PANEL
#define BOX_EDGE         2      // a box's inset hairline (Bild 3: 2 px of text-1 at a few per cent)

// The About card, in the column Bild 3 gives its category list. Its padding is the QR code's margin, so the
// title, the code and the lines under it share one left edge.
#define CARD_X           CONTENT_X
#define CARD_W           380
#define CARD_PAD_X       42
#define CARD_PAD_BOTTOM  30     // the note's last baseline -> the card's bottom edge
#define QR_SCALE         8      // 37 modules incl. quiet zone -> 296 px: the card's whole inner width
#define LINK_GAP         38     // the QR code's bottom -> the link's baseline
#define NOTE_GAP         33     // the link's baseline -> the note's first baseline
#define LINK_SIZE        22

// The options panel, where Bild 3 has its SponsorBlock panel: from x 620 to the safe edge, 44 px of padding
// left and right.
#define PANEL_X          620
#define PANEL_W          (UI_SAFE_RIGHT - PANEL_X)
#define PANEL_PAD_X      44
#define PANEL_PAD_BOTTOM 24
#define TEXT_X           (PANEL_X + PANEL_PAD_X)          // 664
#define TEXT_RIGHT       (UI_SAFE_RIGHT - PANEL_PAD_X)    // 1780
#define SECTION_DY       52     // the Region row's bottom -> the SponsorBlock title's baseline
#define EXPLAIN_DY       40     // that title's baseline -> its explanation's
#define ROWS_DY          33     // the explanation's last baseline -> the first category row

// A row as Bild 3 draws it, the focused one on a plate reaching 18 px past the text either side. Bild 3 steps
// its rows by 82 and makes them 72 tall; nine rows under two section titles only fit inside the safe area at
// a 68-px step, 64 tall - the panel then ends at y 1030, 2 px inside the safe line - which still leaves 4 px
// between rows, and only the focused row ever has a plate. The 60-px segment group and the switch with its
// ring stay inside a 64-px row.
#define ROW_H            64
#define ROW_PITCH        68
#define ROW_TEXT         26     // an option's name, 26/700 - 800 on the focused row
#define ROW_WEIGHT       UI_WEIGHT_BOLD
#define ROW_FOCUS_WEIGHT UI_WEIGHT_EXTRABOLD
#define ROW_PLATE_OUT    18
#define DOT_SIZE         14     // a category's colour, then 16 px to its name
#define DOT_GAP          16
#define NAME_GAP         24     // a name ends in "..." this far short of the control on its right

// Skip / Ask / Keep as one segment group, right-aligned in the panel: 5 px of padding round three 128x50
// segments 4 px apart; radius 18 for the group, 14 for a segment - the radius UI_RING_SEGMENT is made for.
#define SEG_COUNT        3
#define SEG_W            128
#define SEG_H            50
#define SEG_GAP          4
#define SEG_RADIUS       14
#define SEG_TEXT         22
#define GROUP_PAD        5
#define GROUP_W          (2 * GROUP_PAD + SEG_COUNT * SEG_W + (SEG_COUNT - 1) * SEG_GAP)   // 402
#define GROUP_H          (SEG_H + 2 * GROUP_PAD)                                          // 60
#define GROUP_X          (TEXT_RIGHT - GROUP_W)                                           // 1378

// The autoplay switch from the Ember building blocks (Bild 6): 76x42, a 32-px knob 5 px in, right-aligned
// like the groups, with its state in words beside it. Focused, it gets the segments' white ring: 3 px wide,
// 3 px out. ring-r34-w4 scaled by 27/34 follows a pill of radius 21 at that distance - its outer radius
// comes to 21 + 6 and its stroke to 3.2 px - where the segment's own ring, radius 20, would pinch the pill's
// round ends.
#define SWITCH_W         76
#define SWITCH_H         42
#define SWITCH_KNOB      32
#define SWITCH_INSET     5
#define SWITCH_WORD_GAP  14
#define SWITCH_RING_OUT  6
#define SWITCH_RING_SCALE (27.0f / 34.0f)

// The Region row's value: the country's name on a single chosen segment, in a group box of its own like the
// skip categories' - so it reads as a value, the way the lit segment does. The segment is as wide as the
// longest name needs, so it keeps its width whichever country is in use.
#define REGION_PAD       24     // the name -> the segment's edges, at the least

// The words on the controls are rendered once in white and tinted when drawn (as the rail's names are):
// a segment's word takes three colours by state, and re-colouring a label would rasterise it again.
#define TEXT_TINTABLE    0xFFFFFFFFu

// Row order. The playback options come first because they are the ones changed often; the skip categories
// are a block of their own, which is also the only place "skip the intro" can be turned on. Region is no
// toggle: X opens its picker.
enum {
   ROW_AUTOPLAY,
   ROW_REGION,
   ROW_SKIP_FIRST,
   ROW_SKIP_END = ROW_SKIP_FIRST + SPONSOR_CATEGORY_COUNT,   // one past the last skip category
   ROW_COUNT = ROW_SKIP_END
};

static Label       titleLabel, versionLabel;
static Label       aboutLabel, linkLabel, noteLabel;
static Label       playbackLabel, sponsorLabel, explainLabel;
static Label       rowLabels[ROW_COUNT][2];       // each option's name at 700 [0] and, focused, 800 [1]
static Label       segmentLabels[SEG_COUNT][2];   // Skip / Ask / Keep in white, at 700 [0] and 800 [1]
static Label       switchLabels[2];               // "Off" [0] / "On" [1] in white
static Label       regionLabel;                   // the country in use, in white at 800 like a lit segment's word
static UiHints     hints;
static ButtonRepeat navigateRepeat;

static struct {
   int screenW, screenH;
   int selected;
   // the layout, worked out once from the measured text (see layOut)
   int titleY, versionBaseline, clockBaseline;
   int autoplayY, regionY, sponsorBaseline, explainBaseline, categoryY, panelH;
   int linkBaseline, noteBaseline, cardH;
   int regionSegW;        // the Region value's segment: the longest country name and its padding
   uint32_t boxEdge;      // the hairline blended over surface-1, opaque
} state;

static int skipCategory(int row) { return row - ROW_SKIP_FIRST; }
static int isSkipRow(int row)    { return row >= ROW_SKIP_FIRST && row < ROW_SKIP_END; }

// The segment a category's setting lights up: Skip | Ask | Keep, left to right. "Ask" is the old "Skip by
// choice" (SPONSOR_ACTION_ASK): a skip button for as long as the segment runs.
static int segmentOf(int action)
{
   if (action == SPONSOR_ACTION_SKIP) return 0;
   if (action == SPONSOR_ACTION_ASK)  return 1;
   return 2;
}

// X cycles a skip category Keep -> Skip -> Skip by choice -> Keep. Skip comes first because it is what
// most people want from a sponsor segment; "by choice" is the deliberate extra press. With the segments in
// the order Skip | Ask | Keep, every press moves the lit one a step to the right, and Keep comes round to Skip.
// X on Region opens its picker instead (updateSettings), so it never gets here.
static void toggleRow(int row)
{
   if (row == ROW_AUTOPLAY) setAutoplay(!getAutoplay());
   else if (isSkipRow(row)) {
      int category = skipCategory(row);
      int next;
      switch (getSponsorAction(category)) {
      case SPONSOR_ACTION_KEEP: next = SPONSOR_ACTION_SKIP; break;
      case SPONSOR_ACTION_SKIP: next = SPONSOR_ACTION_ASK;  break;
      default:                  next = SPONSOR_ACTION_KEEP; break;
      }
      setSponsorAction(category, next);
   }
}

// The baseline of a label's last line, given its first: every line after the first adds one pitch to the
// texture, so the texture's height says how many there are.
static int lastBaselineOf(const Label *l, int firstBaseline)
{
   UiTextMetrics m = uiGetTextMetrics(l->font, l->size);
   int extra = l->tt.tex.h - m.texH;
   return firstBaseline + (extra > 0 ? extra : 0);
}

// Everything under the header hangs off BODY_Y. The explanation and the note are wrapped labels; their
// measured height places what follows them, so should either ever take another line, the rows move down
// with it instead of running into it.
static void layOut(void)
{
   state.titleY          = uiLabelYForCenter(&titleLabel, HEADER_Y, HEADER_H);
   state.versionBaseline = state.titleY + uiGetTextMetrics(titleLabel.font, titleLabel.size).baseline + VERSION_DY;
   Font *clockFont       = uiGetFont(CLOCK_WEIGHT);
   state.clockBaseline   = uiTextYForCenter(clockFont, CLOCK_SIZE, HEADER_Y, HEADER_H)
                           + uiGetTextMetrics(clockFont, CLOCK_SIZE).baseline;

   state.autoplayY       = BODY_Y + FIRST_ROW_DY;
   state.regionY         = state.autoplayY + ROW_PITCH;
   state.sponsorBaseline = state.regionY + ROW_H + SECTION_DY;
   state.explainBaseline = state.sponsorBaseline + EXPLAIN_DY;
   state.categoryY       = lastBaselineOf(&explainLabel, state.explainBaseline) + ROWS_DY;
   state.panelH          = state.categoryY + (ROW_SKIP_END - 1 - ROW_SKIP_FIRST) * ROW_PITCH + ROW_H
                           + PANEL_PAD_BOTTOM - BODY_Y;

   int qrSpan = (QR_SIZE + 2 * QR_QUIET) * QR_SCALE;
   state.linkBaseline = BODY_Y + FIRST_ROW_DY + qrSpan + LINK_GAP;
   state.noteBaseline = state.linkBaseline + NOTE_GAP;
   state.cardH        = lastBaselineOf(&noteLabel, state.noteBaseline) + CARD_PAD_BOTTOM - BODY_Y;

}

static int rowY(int row)
{
   if (row == ROW_AUTOPLAY) return state.autoplayY;
   if (row == ROW_REGION)   return state.regionY;
   return state.categoryY + (row - ROW_SKIP_FIRST) * ROW_PITCH;
}

// The Region value's segment: wide enough for the longest country name at the lit segment's 22/800, so it
// does not change size with the country - measured, not rendered, once per opening.
static int measureRegionSegment(void)
{
   Font *font = uiGetFont(ROW_FOCUS_WEIGHT);
   int widest = 0;
   for (int i = 0; i < getContentRegionCount(); i++) {
      const ContentRegion *region = getContentRegionAt(i);
      int width = region ? (int)measureFontText(font, SEG_TEXT, region->name) + 1 : 0;
      if (width > widest) widest = width;
   }
   int segment = widest + 2 * REGION_PAD;
   return segment > SEG_W ? segment : SEG_W;
}

// The name of the country in use. getContentRegionIndex is always an index into the table, so the code (never
// NULL) only stands in should that ever change.
static const char *regionName(void)
{
   const ContentRegion *region = getContentRegionAt(getContentRegionIndex());
   return region ? region->name : getContentRegionCode();
}

static void initSettings(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   state.selected = 0;
   // the panels' 2-px edge: the hairline over surface-1, worked out once and drawn opaque (see drawBox)
   state.boxEdge = interpolateColor(activeTheme->surface1, uiWithAlpha(activeTheme->hairline, 0xFF),
                                    (float)(activeTheme->hairline >> 24) / 255.0f);

   // Fonts are shared app-wide (ui-fonts.h): nothing is opened here and nothing may be closed in term.
   // All of this screen's text is written in the code, so it is Manrope throughout.
   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, AUTO, UI_PAGE_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Settings");
   // (V3.0.0 Test 12) with a newer release out, the line says so (update-check.h)
   char version[96], newer[16];
   if (getNewerRelease(newer, sizeof newer)) snprintf(version, sizeof version, "TEE Vanced %s  \xC2\xB7  v%s is out on GitHub",
                                                       APP_VERSION, newer);
   else snprintf(version, sizeof version, "TEE Vanced %s", APP_VERSION);
   uiInitLabel(&versionLabel, UI_META_WEIGHT, CONTENT_X, 0, AUTO, UI_META_SIZE, activeTheme->textTertiary,
               TEXT_NOWRAP, version);

   uiInitLabel(&aboutLabel,    HEADING_WEIGHT, 0, 0, AUTO, HEADING_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "About");
   uiInitLabel(&playbackLabel, HEADING_WEIGHT, 0, 0, AUTO, HEADING_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Playback");
   uiInitLabel(&sponsorLabel,  HEADING_WEIGHT, 0, 0, AUTO, HEADING_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "SponsorBlock");
   // one line across the panel (about 1050 px of the 1116 there are); wrapped rather than cut should a font
   // ever draw it wider, and the rows under it follow its height (layOut)
   uiInitLabel(&explainLabel, UI_BODY_WEIGHT, 0, 0, TEXT_RIGHT - TEXT_X, UI_BODY_SIZE, activeTheme->textSecondary,
               TEXT_WRAP, "What happens when a marked segment starts. Ask shows a skip button for as long as the segment runs.");

   // the card: raw labels, because the URL is literal text, not markup (it is ASCII, so uiInitTextLabel keeps
   // it in Manrope). The note is about 335 px, wider than the code, so it takes two lines - broken by hand,
   // as a plain wrap would leave "contact" alone on the second.
   int cardTextW = CARD_W - 2 * CARD_PAD_X;
   uiInitTextLabel(&linkLabel, UI_WEIGHT_BOLD, 0, 0, cardTextW, LINK_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, QR_URL);
   uiInitTextLabel(&noteLabel, UI_META_WEIGHT, 0, 0, cardTextW, UI_META_SIZE, activeTheme->textSecondary,
                   TEXT_WRAP, "Scan for links, streams\nand contact");

   // The names end short of the segment group, so even "Interaction reminder" (about 270 px at 26/800) never
   // meets it - it has some 660 px. Autoplay's, without a dot, starts 30 px further left, far from its switch.
   // Each name is rendered in both weights here, once: the focused row's is 800 (Bild 3), and swapping
   // textures as the focus moves costs nothing, where restyling a label would rasterise it again - and wait
   // for the RSX - twice for every step of a held d-pad.
   int nameW = GROUP_X - NAME_GAP - (TEXT_X + DOT_SIZE + DOT_GAP);
   for (int row = 0; row < ROW_SKIP_END; row++) {
      const char *name;
      if      (row == ROW_AUTOPLAY) name = "Autoplay next video";
      else if (row == ROW_REGION)   name = "Region";
      else                          name = getSponsorCategoryName((SponsorCategory)skipCategory(row));
      uiInitLabel(&rowLabels[row][0], ROW_WEIGHT,       0, 0, nameW, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, name);
      uiInitLabel(&rowLabels[row][1], ROW_FOCUS_WEIGHT, 0, 0, nameW, ROW_TEXT, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, name);
   }

   // a segment's word: onAccent on the accent, textPrimary when chosen, textSecondary otherwise (drawSegments)
   static const char *SEGMENT_WORDS[SEG_COUNT] = { "Skip", "Ask", "Keep" };
   for (int k = 0; k < SEG_COUNT; k++) {
      uiInitLabel(&segmentLabels[k][0], ROW_WEIGHT,       0, 0, AUTO, SEG_TEXT, TEXT_TINTABLE, TEXT_NOWRAP, SEGMENT_WORDS[k]);
      uiInitLabel(&segmentLabels[k][1], ROW_FOCUS_WEIGHT, 0, 0, AUTO, SEG_TEXT, TEXT_TINTABLE, TEXT_NOWRAP, SEGMENT_WORDS[k]);
   }
   uiInitLabel(&switchLabels[0], ROW_WEIGHT, 0, 0, AUTO, SEG_TEXT, TEXT_TINTABLE, TEXT_NOWRAP, "Off");
   uiInitLabel(&switchLabels[1], ROW_WEIGHT, 0, 0, AUTO, SEG_TEXT, TEXT_TINTABLE, TEXT_NOWRAP, "On");
   // the country names are the app's own (English, ASCII): Manrope, like every other word on this page
   uiInitLabel(&regionLabel, ROW_FOCUS_WEIGHT, 0, 0, AUTO, SEG_TEXT, TEXT_TINTABLE, TEXT_NOWRAP, regionName());
   state.regionSegW = measureRegionSegment();

   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "Change");
   uiAddHint(&hints, GLYPH_CIRCLE, "Back");
   uiAddHint(&hints, GLYPH_DPAD_LEFT, "Menu");   // the rail opens here as on Home

   memset(&navigateRepeat, 0, sizeof navigateRepeat);
   layOut();
}

// Back from the region picker. A direction still held as it closed must wait the normal hold delay here
// rather than fire on the first frame: a timer last stamped before the picker opened reads as long overdue.
static void resumeSettings(void)
{
   navigateRepeat.timer   = sys_time_get_system_time();
   navigateRepeat.repeats = 0;
}

static void updateSettings(void)
{
   // The rail: LEFT opens it here as on Home (home.h). Settings picked in it is this screen: the rail just
   // closes, and a direction still held then waits out the normal delay, as after the picker.
   int rail = updateRailOverScreen(SIDE_SETTINGS);
   if (rail == RAIL_GONE) return;   // Home is back on top: this screen is gone, touch nothing of it
   if (rail != RAIL_NOT_OPEN) {
      if (!isSidebarFocused()) resumeSettings();
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }

   if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) state.selected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   state.selected--;
   if (state.selected < 0) state.selected = ROW_COUNT - 1;            // wrap: the list is short enough that
   if (state.selected >= ROW_COUNT) state.selected = 0;               // running off an end should come round

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (state.selected == ROW_REGION) { openRegionScreen(); return; }   // a list to pick from, not a toggle
      toggleRow(state.selected);
   }
}

// a label rendered in TEXT_TINTABLE, drawn in `color`
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

// An Ember panel (Bild 3): surface-1 with a 2-px inset hairline, drawn as two opaque rounded fills - the edge
// colour, then the surface 2 px in - so the edge is exactly a CSS inset border and nothing behind shows
// through it.
static void drawBox(int x, int y, int w, int h)
{
   uiFillRounded(x, y, w, h, BOX_RADIUS, state.boxEdge);
   uiFillRounded(x + BOX_EDGE, y + BOX_EDGE, w - 2 * BOX_EDGE, h - 2 * BOX_EDGE, BOX_RADIUS - BOX_EDGE,
                 activeTheme->surface1);
}

// The values are re-read from storage on every draw, so the screen never holds its own copy of a setting
// (which is how a toggle ends up disagreeing with what was actually saved). A value says what it is by its
// look as much as by its word: the chosen segment is filled, the others are muted text - eight rows of grey
// words all look alike from a sofa. On the focused row the chosen one is the solid accent with the white ring
// round it, which is what X will move.
static void drawSegments(int row, int y, int focused)
{
   int chosen = segmentOf(getSponsorAction(skipCategory(row)));
   int groupY = y + (ROW_H - GROUP_H) / 2, segY = groupY + GROUP_PAD;
   uiFillRounded(GROUP_X, groupY, GROUP_W, GROUP_H, UI_RADIUS_ROW, activeTheme->appBg);

   int chosenX = 0;
   for (int k = 0; k < SEG_COUNT; k++) {
      int segX = GROUP_X + GROUP_PAD + k * (SEG_W + SEG_GAP);
      uint32_t color = activeTheme->textSecondary;
      if (k == chosen) {
         uiFillRounded(segX, segY, SEG_W, SEG_H, SEG_RADIUS, focused ? activeTheme->accent : activeTheme->surface3);
         color = focused ? activeTheme->onAccent : activeTheme->textPrimary;
         chosenX = segX;
      }
      const Label *word = &segmentLabels[k][k == chosen ? 1 : 0];
      drawTinted(word, segX + (SEG_W - word->tt.tex.w) / 2, uiLabelYForCenter(word, segY, SEG_H), color);
   }
   if (focused) uiDrawFocusRing(chosenX, segY, SEG_W, SEG_H, UI_RING_SEGMENT, activeTheme->textPrimary);
}

// The switch: on, the accent with the dark knob at its right; off, surface-3 with a text-3 knob at its left -
// the knob's side says it as much as the colour does, and "On" / "Off" beside it say it in words.
static void drawSwitch(int y, int focused)
{
   int on = getAutoplay() ? 1 : 0;
   int x = TEXT_RIGHT - SWITCH_W, switchY = y + (ROW_H - SWITCH_H) / 2;
   uiFillPill(x, switchY, SWITCH_W, SWITCH_H, on ? activeTheme->accent : activeTheme->surface3);
   int knobX = on ? x + SWITCH_W - SWITCH_INSET - SWITCH_KNOB : x + SWITCH_INSET;
   uiFillCircle(knobX, switchY + (SWITCH_H - SWITCH_KNOB) / 2, SWITCH_KNOB, on ? activeTheme->onAccent : activeTheme->textTertiary);

   if (focused) {
      int ringX = x - SWITCH_RING_OUT, ringY = switchY - SWITCH_RING_OUT;
      int ringW = SWITCH_W + 2 * SWITCH_RING_OUT, ringH = SWITCH_H + 2 * SWITCH_RING_OUT;
      if (uiGetTexture(UI_TEX_RING_R34_W4).offset)
         uiDrawNineSlice(UI_TEX_RING_R34_W4, ringX, ringY, ringW, ringH, SWITCH_RING_SCALE, activeTheme->textPrimary);
      else
         strokeGfxRectangle(ringX, ringY, ringW, ringH, 3, activeTheme->textPrimary);   // as uiDrawFocusRing falls back
   }

   const Label *word = &switchLabels[on];
   drawTinted(word, x - SWITCH_WORD_GAP - word->tt.tex.w, uiLabelYForCenter(word, y, ROW_H),
              on ? activeTheme->textPrimary : activeTheme->textSecondary);
}

// The Region value: the country in use as a lit segment in a group of its own, right-aligned like the skip
// categories' groups - surface-3 at rest, on the focused row the accent with the white ring, which is what X
// opens. The name is re-read on every draw like the other values; the label only renders when it changes,
// i.e. on the first frame back from the picker.
static void drawRegion(int y, int focused)
{
   setLabelText(&regionLabel, regionName());
   int segW   = state.regionSegW;
   int groupW = segW + 2 * GROUP_PAD, groupX = TEXT_RIGHT - groupW;
   int groupY = y + (ROW_H - GROUP_H) / 2, segX = groupX + GROUP_PAD, segY = groupY + GROUP_PAD;
   uiFillRounded(groupX, groupY, groupW, GROUP_H, UI_RADIUS_ROW, activeTheme->appBg);
   uiFillRounded(segX, segY, segW, SEG_H, SEG_RADIUS, focused ? activeTheme->accent : activeTheme->surface3);
   drawTinted(&regionLabel, segX + (segW - regionLabel.tt.tex.w) / 2, uiLabelYForCenter(&regionLabel, segY, SEG_H),
              focused ? activeTheme->onAccent : activeTheme->textPrimary);
   if (focused) uiDrawFocusRing(segX, segY, segW, SEG_H, UI_RING_SEGMENT, activeTheme->textPrimary);
}

static void drawRow(int row)
{
   int y = rowY(row), focused = (row == state.selected);
   if (focused)   // the plate that says "you are here", as Bild 3 lays it under the focused row
      uiFillRounded(TEXT_X - ROW_PLATE_OUT, y, TEXT_RIGHT - TEXT_X + 2 * ROW_PLATE_OUT, ROW_H, UI_RADIUS_ROW,
                    activeTheme->hintFill);

   int nameX = TEXT_X;
   if (isSkipRow(row)) {
      // the category's colour, the one its segments wear on the player's timeline
      uiFillCircle(TEXT_X, y + (ROW_H - DOT_SIZE) / 2, DOT_SIZE,
                   getSponsorCategoryColor((SponsorCategory)skipCategory(row)));
      nameX += DOT_SIZE + DOT_GAP;
   }
   const Label *name = &rowLabels[row][focused];
   uiDrawLabelAt(name, nameX, uiLabelYForCenter(name, y, ROW_H));

   if      (row == ROW_AUTOPLAY) drawSwitch(y, focused);
   else if (row == ROW_REGION)   drawRegion(y, focused);
   else                          drawSegments(row, y, focused);
}

static void drawCard(void)
{
   int x = CARD_X + CARD_PAD_X;
   drawBox(CARD_X, BODY_Y, CARD_W, state.cardH);
   uiDrawLabelAt(&aboutLabel, x, uiLabelYForBaseline(&aboutLabel, BODY_Y + HEADING_DY));
   // level with the Autoplay row, so the card and the panel read as one grid
   drawQrCode(x, BODY_Y + FIRST_ROW_DY, QR_SCALE);
   uiDrawLabelAt(&linkLabel, x, uiLabelYForBaseline(&linkLabel, state.linkBaseline));
   uiDrawLabelAt(&noteLabel, x, uiLabelYForBaseline(&noteLabel, state.noteBaseline));
}

static void drawPanel(void)
{
   drawBox(PANEL_X, BODY_Y, PANEL_W, state.panelH);
   uiDrawLabelAt(&playbackLabel, TEXT_X, uiLabelYForBaseline(&playbackLabel, BODY_Y + HEADING_DY));
   uiDrawLabelAt(&sponsorLabel,  TEXT_X, uiLabelYForBaseline(&sponsorLabel, state.sponsorBaseline));
   uiDrawLabelAt(&explainLabel,  TEXT_X, uiLabelYForBaseline(&explainLabel, state.explainBaseline));
   for (int row = 0; row < ROW_SKIP_END; row++) drawRow(row);
}

static void drawSettings(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   if (!isSidebarFocused()) drawSidebarPassive(state.screenH, SIDE_SETTINGS);   // open, it is drawn last (below)

   uiDrawLabelAt(&titleLabel, CONTENT_X, state.titleY);
   uiDrawLabelAt(&versionLabel, CONTENT_X, uiLabelYForBaseline(&versionLabel, state.versionBaseline));
   uiDrawClock(UI_SAFE_RIGHT, state.clockBaseline);

   drawCard();
   drawPanel();
   // bottom left, under the About card, where the Ember settings design (Bild 3) puts this screen's hints
   uiDrawHints(&hints, CONTENT_X, UI_HINTS_BOTTOM_Y, UI_ALIGN_LEFT);
   // The rail, open, goes over everything - the hints included: they start at x 176, inside the open panel,
   // and drawn over it they would run into its names. Its scrim dims them with the rest of the page.
   if (isSidebarFocused()) drawSidebar(state.screenH);
}

static void termSettings(void)
{
   uiTermHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&versionLabel);
   freeLabel(&aboutLabel);
   freeLabel(&linkLabel);
   freeLabel(&noteLabel);
   freeLabel(&playbackLabel);
   freeLabel(&sponsorLabel);
   freeLabel(&explainLabel);
   for (int row = 0; row < ROW_SKIP_END; row++) { freeLabel(&rowLabels[row][0]); freeLabel(&rowLabels[row][1]); }
   for (int k = 0; k < SEG_COUNT; k++) { freeLabel(&segmentLabels[k][0]); freeLabel(&segmentLabels[k][1]); }
   freeLabel(&switchLabels[0]);
   freeLabel(&switchLabels[1]);
   freeLabel(&regionLabel);
   // no closeFont: the fonts belong to ui-fonts.c and every other screen is still drawing with them
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen settingsScreen = { initSettings, resumeSettings, updateSettings, drawSettings, NULL, termSettings,
                          SCREEN_TERMINATED };

void openSettingsScreen(void) { pushScreen(&settingsScreen); }

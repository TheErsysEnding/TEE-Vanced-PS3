// sidebar - implementation (see sidebar.h). Modelled on apps/swarm's chrome.c, which already drives a left
// icon rail with this exact focus model.

#include "sidebar.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "ui-fonts.h"         // uiInitLabel / uiLabelYForCenter: the entry names are Manrope like all UI text
#include "ui-draw.h"          // uiWithAlpha
#include <string.h>

// The first entry is level with the first row of home's list. It used to sit at 96, just under a 26-px
// heading at the top of the screen; home's Ember heading is 46 px, on the line of the top-right hint row,
// and drawn OVER the expanded panel (so "Loading..." stays visible) - at 96 the first entry's highlight and
// name would run into its lower half.
#define ITEM_TOP     140
#define ITEM_STEP    88
#define ICON_SIZE    32
#define LABEL_SIZE   24       // Ember's panel entries: 24/700
#define LABEL_WEIGHT UI_WEIGHT_BOLD
#define PANEL_WIDTH  330      // the expanded overlay, drawn only while focused - at least this wide
#define LABEL_X      (SIDEBAR_WIDTH + 6)
#define LABEL_PAD    16       // panel room after the longest name
#define MARK_WIDTH   5        // the accent bar on the current entry
// The names are rendered once in white and tinted when drawn, the way the icons beside them are: the tint
// follows the highlight, and re-colouring a rendered label means rasterising it again - which setLabelColor
// used to do in the draw path on every move of the highlight.
#define TEXT_TINTABLE 0xFFFFFFFFu

// Four of the eight have an exact glyph in the embedded set (search, clock, history, cog). The other four
// have none - there is no play, trending, subscription or playlist symbol in it - so they borrow the
// nearest honest reading rather than a made-up one: an up arrow for what is rising, a star for channels you
// follow, a right arrow for picking a video back up, stacked pages for lists of videos.
static const IconId ITEM_ICONS[SIDE_COUNT] = {
   ICON_SEARCH,      // Search
   ICON_UP_BIG,      // Trending
   ICON_STAR,        // Subscriptions
   ICON_RIGHT_BIG,   // Continue watching
   ICON_DOCS,        // Playlists
   ICON_CLOCK,       // Watch Later
   ICON_HISTORY,     // History
   ICON_COG,         // Settings
};

static const char *ITEM_NAMES[SIDE_COUNT] = {
   "Search", "Trending", "Subscriptions", "Continue watching",
   "Playlists", "Watch Later", "History", "Settings",
};

static Icon  icons[SIDE_COUNT];
static Label labels[SIDE_COUNT];
static int   current = SIDE_SUBS;   // what the content is showing
static int   picked  = SIDE_SUBS;   // where the highlight sits while focused
static int   focused;
static int   panelWidth = PANEL_WIDTH;   // PANEL_WIDTH, or wider when the longest name needs it

int isSidebarItemEnabled(SidebarItem item) { (void)item; return 1; }   // every entry leads somewhere now
const char *sidebarItemName(SidebarItem item)
{
   return (item >= 0 && item < SIDE_COUNT) ? ITEM_NAMES[item] : "";
}

void initSidebar(void)
{
   focused = 0;
   current = picked = SIDE_SUBS;
   panelWidth = PANEL_WIDTH;

   for (int i = 0; i < SIDE_COUNT; i++) {
      initIcon(&icons[i], ITEM_ICONS[i], ICON_SIZE);
      uiInitLabel(&labels[i], LABEL_WEIGHT, LABEL_X, 0, AUTO, LABEL_SIZE, TEXT_TINTABLE, TEXT_NOWRAP, ITEM_NAMES[i]);
      // The panel takes its longest name whole. "Continue watching" in Manrope 24 no longer fits the old
      // 330, and an ellipsis in a menu of eight fixed words would only hide which entry it is.
      int needed = LABEL_X + labels[i].tt.tex.w + LABEL_PAD;
      if (needed > panelWidth) panelWidth = needed;
   }
}

void termSidebar(void)
{
   for (int i = 0; i < SIDE_COUNT; i++) { freeIcon(&icons[i]); freeLabel(&labels[i]); }
}

void setSidebarCurrent(SidebarItem item)
{
   if (item < 0 || item >= SIDE_COUNT) return;
   current = item;
   if (!focused) picked = item;   // while unfocused the highlight simply follows what is on screen
}

int isSidebarFocused(void) { return focused; }

void setSidebarFocused(int focus)
{
   focused = focus ? 1 : 0;
   if (focused) picked = current;   // always open on the entry you are actually looking at
}

static int step(int from, int delta)
{
   int next = from + delta;
   if (next < 0) next = SIDE_COUNT - 1;
   if (next >= SIDE_COUNT) next = 0;
   return next;
}

int updateSidebar(void)
{
   if (!focused) return -1;

   if (isPadButtonPressed(PAD_BTN_DOWN)) picked = step(picked, 1);
   if (isPadButtonPressed(PAD_BTN_UP))   picked = step(picked, -1);

   // O leaves the rail without changing anything - the one gesture that must never commit by accident
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { focused = 0; return -1; }

   if (isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_RIGHT)) return picked;
   return -1;
}

static int itemY(int index) { return ITEM_TOP + index * ITEM_STEP; }

// a name (rendered in TEXT_TINTABLE) drawn in `color`, alpha included
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

// where an entry's name goes: its capitals centred on the icon's ink, which is what the eye lines it up
// with - the glyph does not fill its em box, so the box's middle is not the icon's
static int labelY(int index, int y)
{
   const Icon *icon = &icons[index];
   if (icon->tex && icon->tex->h > 0) return uiLabelYForCenter(&labels[index], y + icon->offsetY, icon->tex->h);
   return uiLabelYForCenter(&labels[index], y, ICON_SIZE);
}

void drawSidebar(int screenH)
{
   int width = focused ? panelWidth : SIDEBAR_WIDTH;
   // Expanded or collapsed, it runs the full height. The panel used to stop above the button-hint row: that
   // row was centred, wide enough to reach under the panel, and painting over it made the hints look broken.
   // The hint rows are right-aligned now and start far to the right of the panel.
   //
   // The rail is its own surface so it reads as furniture rather than as part of the list. Expanded, it is
   // opaque and sits over the content - the content is never re-laid out.
   fillGfxRectangle(0, 0, width, screenH, activeTheme->surface);
   fillGfxRectangle(width, 0, 2, screenH, activeTheme->accent);

   for (int i = 0; i < SIDE_COUNT; i++) {
      int y = itemY(i);
      int isCurrent = (i == current);
      int isPicked  = focused && i == picked;
      int enabled   = isSidebarItemEnabled(i);

      if (isPicked)       fillGfxRectangle(0, y - 14, width, ITEM_STEP - 12, activeTheme->rowHighlight);
      if (isCurrent)      fillGfxRectangle(0, y - 14, MARK_WIDTH, ITEM_STEP - 12, activeTheme->accent);

      uint32_t tint = !enabled  ? activeTheme->textSecondary
                    : isPicked  ? activeTheme->accent
                    : isCurrent ? activeTheme->textPrimary
                                : activeTheme->textSecondary;
      // a disabled entry is drawn at half strength on top of the muted colour, so it reads as "not yet"
      if (enabled) drawIcon(&icons[i], SIDEBAR_WIDTH / 2 - ICON_SIZE / 2, y, tint);
      else         drawIconAlpha(&icons[i], SIDEBAR_WIDTH / 2 - ICON_SIZE / 2, y, tint, 110);

      if (focused) {
         drawTinted(&labels[i], LABEL_X, labelY(i, y), enabled ? tint : uiWithAlpha(tint, 110));
      }
   }
}

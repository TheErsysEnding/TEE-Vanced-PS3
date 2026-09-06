// sidebar - implementation (see sidebar.h). Modelled on apps/swarm's chrome.c, which already drives a left
// icon rail with this exact focus model.

#include "sidebar.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include <string.h>

#define ITEM_TOP     96
#define ITEM_STEP    88
#define ICON_SIZE    32
#define LABEL_SIZE   23
#define PANEL_WIDTH  330      // the expanded overlay, drawn only while focused
#define LABEL_X      (SIDEBAR_WIDTH + 6)
#define MARK_WIDTH   5        // the accent bar on the current entry

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
static int   panelHeight;

int isSidebarItemEnabled(SidebarItem item) { return item != SIDE_PLAYLISTS; }
const char *sidebarItemName(SidebarItem item)
{
   return (item >= 0 && item < SIDE_COUNT) ? ITEM_NAMES[item] : "";
}

void initSidebar(Font *font, int screenH)
{
   focused = 0;
   current = picked = SIDE_SUBS;
   panelHeight = ITEM_TOP + SIDE_COUNT * ITEM_STEP + 20;
   if (panelHeight > screenH - 40) panelHeight = screenH - 40;

   for (int i = 0; i < SIDE_COUNT; i++) {
      initIcon(&icons[i], ITEM_ICONS[i], ICON_SIZE);
      initLabel(&labels[i], font, 0, 0, PANEL_WIDTH - LABEL_X - 16, AUTO, LABEL_SIZE,
                activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, ITEM_NAMES[i]);
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

void drawSidebar(int screenH)
{
   int width = focused ? PANEL_WIDTH : SIDEBAR_WIDTH;
   // Expanded, the panel stops above the button-hint row: it is wide enough to reach the left end of that
   // centred row, and painting over it made the hints look broken. Collapsed, the strip is narrow enough
   // that the row never reaches it, so it runs the full height and reads as part of the frame.
   int height = focused ? panelHeight : screenH;

   // The rail is its own surface so it reads as furniture rather than as part of the list. Expanded, it is
   // opaque and sits over the content - the content is never re-laid out.
   fillGfxRectangle(0, 0, width, height, activeTheme->surface);
   fillGfxRectangle(width, 0, 2, height, activeTheme->accent);

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
         setLabelColor(&labels[i], tint);
         moveLabel(&labels[i], LABEL_X, y + (ICON_SIZE - labels[i].tt.tex.h) / 2 + 2);
         if (enabled) drawLabel(&labels[i]);
         else         drawLabelAlpha(&labels[i], 110);
      }
   }
}

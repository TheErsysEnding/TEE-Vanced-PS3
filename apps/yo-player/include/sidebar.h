#pragma once

// sidebar - the icon rail down the left edge of the home screen.
//
// It exists so the app stops being driven by shoulder buttons nobody can guess. Every place you can go is
// on screen as a symbol; the ones that used to need L1/R1, START or a remembered button now need only the
// d-pad. That also hands buttons back: with Search living here, START is free for the settings.
//
// Collapsed it is Ember's 112-px icon rail (Bild 1 and 3): the orange "TEE" tile at the top, one icon per
// entry, Settings at the foot, and the entry the content belongs to on a 64x64 accent-soft square with an
// orange icon. Focused it draws the labelled panel of Bild 4 and 5 OVER the content rather than pushing it
// aside - reflowing the grid would re-lay out every tile and re-decode its thumbnail each time the focus
// moved, for a list that is about to be replaced anyway - and dims everything to its right with the scrim,
// so whatever the panel half covers reads as behind it. The icons stay exactly where they were.
//
// Icons come from simple-lib-app's embedded icon font (ui/icon-font.h): a Fontello TTF compiled into the
// library, rasterised once per glyph and tinted per draw. Nothing is shipped, nothing is loaded from disk.
// The PS3's own font has no magnifier, clock or gear - only the 16 controller glyphs the app already uses.
// The entry names are fixed UI text in the shared Manrope (ui-fonts.h); the rail opens no font of its own.

#define SIDEBAR_WIDTH      112   // the collapsed rail; only it may reach the screen's edge
#define SIDEBAR_CONTENT_X  176   // where a page's content starts next to the collapsed rail (UEBERGABE 3.4)

// Order is top-to-bottom on screen. Search first because it is the one you reach for cold; Settings last
// because it is the one you reach for rarely.
typedef enum {
   SIDE_SEARCH,
   SIDE_TRENDING,
   SIDE_SUBS,
   SIDE_CONTINUE,
   SIDE_PLAYLISTS,
   SIDE_LATER,
   SIDE_HISTORY,
   SIDE_SETTINGS,
   SIDE_COUNT
} SidebarItem;

void initSidebar(void);
void termSidebar(void);

// which entry the content on screen belongs to - drives the "you are here" mark
void setSidebarCurrent(SidebarItem item);

int  isSidebarFocused(void);
void setSidebarFocused(int focused);   // entering also parks the highlight on the current entry

// Runs only while focused. UP/DOWN move the highlight, X or RIGHT pick, O leaves without changing anything.
// Returns the picked entry, or -1 when nothing was picked this frame.
int  updateSidebar(void);

// the rail (or, while focused, the expanded panel and the scrim right of it) down the full height of the
// screen
void drawSidebar(int screenH);

// The collapsed rail with active marked, for screens that show the rail but never give it focus. Reads no
// input. initSidebar() has run: home initialises the rail at start and every such screen opens over home.
void drawSidebarPassive(int screenH, SidebarItem active);

// Kept for entries that may be added before they work: a dimmed, unpickable row says what is coming without
// pretending it is here. Everything currently listed leads somewhere, so this returns 1 throughout.
int  isSidebarItemEnabled(SidebarItem item);
const char *sidebarItemName(SidebarItem item);

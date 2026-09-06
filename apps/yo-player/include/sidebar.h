#pragma once

// sidebar - the icon rail down the left edge of the home screen.
//
// It exists so the app stops being driven by shoulder buttons nobody can guess. Every place you can go is
// on screen as a symbol; the ones that used to need L1/R1, START or a remembered button now need only the
// d-pad. That also hands buttons back: with Search living here, START is free for the settings.
//
// Collapsed it is a narrow strip of icons. Focused it draws an expanded panel WITH labels OVER the content
// rather than pushing it aside - reflowing the grid would re-lay out every tile and re-decode its thumbnail
// each time the focus moved, for a list that is about to be replaced anyway.
//
// Icons come from simple-lib-app's embedded icon font (ui/icon-font.h): a Fontello TTF compiled into the
// library, rasterised once per glyph and tinted per draw. Nothing is shipped, nothing is loaded from disk.
// The PS3's own font has no magnifier, clock or gear - only the 16 controller glyphs the app already uses.

#include "font.h"

#define SIDEBAR_WIDTH 92

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

void initSidebar(Font *font, int screenH);
void termSidebar(void);

// which entry the content on screen belongs to - drives the "you are here" mark
void setSidebarCurrent(SidebarItem item);

int  isSidebarFocused(void);
void setSidebarFocused(int focused);   // entering also parks the highlight on the current entry

// Runs only while focused. UP/DOWN move the highlight, X or RIGHT pick, O leaves without changing anything.
// Returns the picked entry, or -1 when nothing was picked this frame.
int  updateSidebar(void);

void drawSidebar(int screenH);

// Playlists has no storage behind it yet, so its entry is drawn dimmed and refuses to be picked. Showing it
// greyed is deliberate: it says what is coming without pretending it is here.
int  isSidebarItemEnabled(SidebarItem item);
const char *sidebarItemName(SidebarItem item);

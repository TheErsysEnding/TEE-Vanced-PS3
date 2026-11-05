#pragma once
#include "screen-manager.h"
#include "sidebar.h"            // SidebarItem

// home - the landing screen and its list pages (Ember's Bild 4): Subscriptions, the trending categories, the
// live streams, Continue watching, Watch Later, History and an opened playlist, each as a page of cards in a
// VideoGrid. L1/R1 walk the pages (on Live only its six chips), the rail on the left (O, or LEFT from the
// leftmost column) reaches them and Search, Playlists and Settings; START opens the settings, Triangle a
// channel, X plays.

void openHome(void);
extern Screen homeScreen;

// The rail on a screen pushed over Home that shows it (Search, the search field, Settings, Playlists): LEFT opens
// it there, and it then owns the pad exactly as on Home - UP/DOWN move, X picks, O or RIGHT only close it. Call it
// first in the screen's update and act on what it returns:
//   RAIL_NOT_OPEN   the rail took nothing this frame: the screen handles its input as usual
//   RAIL_OPEN       the rail took this frame's input (it opened, moved, or O or RIGHT closed it): the screen does
//                   nothing else with the pad this frame
//   RAIL_PICKED_OWN X on the screen's own entry; the rail is closed again, the screen decides what that means
//   RAIL_GONE       another entry was picked: Home is back on top and acts on it as on a pick in its own rail. The
//                   calling screen has been terminated - return from its update at once, touching none of its state.
#define RAIL_NOT_OPEN   0
#define RAIL_OPEN       1
#define RAIL_PICKED_OWN 2
#define RAIL_GONE       3
int updateRailOverScreen(SidebarItem own);

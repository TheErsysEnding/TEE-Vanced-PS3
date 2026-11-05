#pragma once
#include "screen-manager.h"

// home - the landing screen and its list pages (Ember's Bild 4): Subscriptions, the trending categories, the
// live streams, Continue watching, Watch Later, History and an opened playlist, each as a page of cards in a
// VideoGrid. L1/R1 walk the pages (on Live only its six chips), the rail on the left (O, or LEFT from the
// leftmost column) reaches them and Search, Playlists and Settings; START opens the settings, Triangle a
// channel, X plays.

void openHome(void);
extern Screen homeScreen;

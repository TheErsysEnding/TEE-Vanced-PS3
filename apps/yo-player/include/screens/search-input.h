#pragma once

// search-input screen - the app's own search field: the query on top, the searches you made before listed
// underneath, scrollable and pickable.
//
// It exists because the system keyboard has no memory. cellOskDialog offers no option for previously entered
// text (the API has none - see the list in osk-input.c), so unlike the PSN sign-in field, an app's keyboard
// starts blank every time. Keeping the queries here and offering them as a list is the app-side substitute,
// and on a gamepad it is faster than the keyboard would be anyway.
//
// Pushed on top of whichever screen opened it; it pops itself before handing the query back, so the caller
// decides what a query means (home opens a results screen, search re-searches in place).

#include "screen.h"

extern Screen searchInputScreen;

// called once with the chosen query after the screen has closed. Never called if the user backs out.
typedef void (*SearchInputDone)(const char *query);

// open the search field. initialText prefills it (may be NULL); onDone receives the confirmed query.
void openSearchInput(const char *initialText, SearchInputDone onDone);

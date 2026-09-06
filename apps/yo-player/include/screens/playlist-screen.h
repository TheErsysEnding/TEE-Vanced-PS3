#pragma once

// playlist screen - your own lists, and the one place a video is put into one.
//
// Two jobs, one screen, because they are the same list of names with a different verb attached:
//   BROWSE - reached from the rail. Pick a list to open it in the home feed, make a new one, or throw one
//            away. X opens, START creates, Square deletes (with a confirmation, since it cannot be undone).
//   CHOOSE - reached with L3 on a video. The same names plus "New playlist...", and picking one files the
//            video there. Closes itself afterwards, so it never sits between you and what you were doing.
//
// A playlist here is not a YouTube playlist: without a signed-in account there is no such thing to read or
// write. These are yours, kept on the console, and nothing outside the app can change them.

#include "extractor.h"   // SearchResult

// Browse: the callback receives the chosen playlist index, after the screen has popped itself.
void openPlaylists(void (*onOpen)(int playlist));

// Choose: files `item` into whichever list is picked. Takes a copy - the caller's entry may be gone by the
// time the pick happens.
void openPlaylistChooser(const SearchResult *item);

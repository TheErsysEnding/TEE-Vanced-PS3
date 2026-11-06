#pragma once

// playlist screen - your own lists, and the one place a video is put into one.
//
// Two jobs, one screen, because they are the same list of names with a different verb attached:
//   BROWSE - reached from the rail. Pick a list to open it in the home feed, make a new one, or throw one
//            away. X opens, START creates, Square deletes (with a confirmation, since it cannot be undone).
//   CHOOSE - reached with L3 on a video. The same names plus "New playlist...", and picking one files the
//            video there. Closes itself afterwards, so it never sits between you and what you were doing.
//
// These playlists are kept on the console. Signed in to YouTube (account.h), Browse lists the account's own
// playlists above them - Liked videos and the ones made on YouTube - which open the same way; changing those is
// YouTube's business for now (no Delete on them, and the chooser files into the console's lists only).

#include "extractor.h"   // SearchResult

// Browse: onOpen receives the chosen playlist's index, onOpenAccount the chosen YouTube playlist's id and title -
// either after the screen has popped itself.
void openPlaylists(void (*onOpen)(int playlist), void (*onOpenAccount)(const char *id, const char *title));

// Choose: files `item` into whichever list is picked. Takes a copy - the caller's entry may be gone by the
// time the pick happens.
void openPlaylistChooser(const SearchResult *item);

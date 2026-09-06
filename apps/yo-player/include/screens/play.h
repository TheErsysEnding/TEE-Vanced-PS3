#pragma once

// play screen - resolves a video and plays it full-screen. Pushed on top of the screen that started it,
// popped with Circle.

#include "screen.h"
#include "extractor.h"   // SearchResults (autoplay queue)

extern Screen playScreen;

// resolves and plays `input` (a youtube link or 11-char video id): stores it and pushes the play screen on
// top of the current one. Nothing follows it - use playVideoFromList to keep playing afterwards.
void playVideo(const char *input);

// play results->items[index] and, when it ends, carry on with the entries after it (autoplay). Only the
// video ids are kept, so the queue costs a few kB rather than a copy of the whole result set. Autoplay is a
// user setting (getAutoplay); when it is off the queue is simply never advanced.
void playVideoFromList(const SearchResults *results, int index);

#pragma once

// ui-toast - (V3.0.4) a short word at the bottom left of a list page: "Added to Watch Later". TEE asked for a
// confirmation when Square files a video, big enough to be noticed: a plate with an accent edge, the icon and the
// words in 34 px, above the bottom hint row, for 2.5 s, fading out at the end. UI thread only.

#include "ui/icon-ids.h"

// shows `text` (fixed English UI text) with `icon`; a new word replaces the one standing
void uiShowToast(IconId icon, const char *text);

// draws the word while it stands - call it in a screen's draw after the page and its hints, before the rail
void uiDrawToast(void);

// (V3.0.4) Square's word on a video: added, removed - or not added, Watch Later being full
void uiShowWatchLaterToast(int wasIn, int isIn);

// releases its texture (screen teardown); the next uiShowToast makes it again
void uiTermToast(void);

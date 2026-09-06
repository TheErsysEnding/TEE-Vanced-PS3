#pragma once

// app-paths - the one directory everything the user accumulates lives in: watch history and resume
// positions, subscriptions, the watch-later queue, search history, preferences, themes and settings.
//
// Deliberately NOT under /dev_hdd0/tmp. That is where this app used to keep it (and where the rest of
// ps3-dev still does), and it is exactly what the name says: the console treats it as scratch space, so
// the whole lot vanished whenever the package was removed to install a newer build. Updating the app cost
// the user everything they had built up, which is not a trade anyone would agree to.
//
// /dev_hdd0/yo-player is outside the game folder, so uninstalling the package does not touch it either.
#define YO_DATA_DIR   "/dev_hdd0/yo-player"

// where those files used to live; copied across once, on the first run of a build that knows about it.
#define YO_LEGACY_DIR "/dev_hdd0/tmp/yo-player"

// creates YO_DATA_DIR and, on the first run only, brings the old files over from YO_LEGACY_DIR.
// Must run before anything reads a file from either place - i.e. before initTheme/initStorage.
void migrateAppData(void);

// The directory migrateAppData actually settled on, which is NOT always YO_DATA_DIR: the console has
// refused to create /dev_hdd0/yo-player before now, and the code falls back through a candidate list.
// Anything that reads or writes app data must ask for the path rather than build it from the macro -
// theme.c did the latter and spent a release looking in a folder that was not there.
// Valid only after migrateAppData(); returns YO_DATA_DIR until then.
const char *getAppDataDir(void);

// The version shown in the app and on its settings card. BUILD_STAMP is a git count+hash and stays
// identical between builds while the tree is uncommitted, so it cannot tell two packages apart - which
// is exactly what made a shipped-but-stale build hard to identify from a screenshot.
#define APP_VERSION "V0.11 Beta"

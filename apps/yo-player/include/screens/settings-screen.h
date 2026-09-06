#pragma once

// settings screen - the app's options, on screen instead of in a text file.
//
// Everything here already existed in the code and was simply unreachable: SponsorBlock has skipped intros
// and outros since the first build, but the only way to turn that on was to edit settings.txt over FTP, and
// the default (ads) leaves it off. A console app whose features can only be reached with an FTP client does
// not really have those features.
//
// Opened with START from the home screen, or from the Settings entry in the left rail; O closes it.

void openSettingsScreen(void);

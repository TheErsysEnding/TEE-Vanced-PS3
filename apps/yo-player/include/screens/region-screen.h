#pragma once

// region screen - the picker behind Settings > Region: which country's YouTube the lists come from.
//
// One row per country of the fixed table in settings.h (getContentRegionAt), the one in use marked with a
// check. UP/DOWN move (and wrap), L2/R2 jump a screenful, X takes the country and closes, O closes without
// changing anything; nothing else reacts. The pick is in use from the next request and kept in settings.txt
// (setContentRegionIndex) - Home notices it on its own when it is back on top (getContentRegionRevision).
//
// Pushed only from the settings screen, which only Home pushes: no list can be loading while it is open.

void openRegionScreen(void);

#pragma once
// update-check - is a newer TEE Vanced out? (V3.0.0 Test 12, asked for on PSX-Place: "check for an update in
// options, or a notification at boot, as in Movian")
//
// Once a start, on a worker, the app reads the top of the CHANGELOG.md in the main branch of its GitHub repository:
// its first "## vX.Y.Z" heading is the newest release. raw.githubusercontent.com is used because its certificate
// chains to ISRG Root X1, which the app carries; api.github.com's Sectigo chain it does not. A release newer than this
// build's own version (APP_VERSION, a test build counting as the version it leads to) is reported to Home, which
// says so once, and to Settings, under the version. Nothing is downloaded or installed. settings.txt update-check=off
// leaves GitHub alone.

void startUpdateCheck(void);   // spawns the one check (call once, after loadSettings and the network)
void stopUpdateCheck(void);    // joins it (app exit)

// 1 once the check found a newer release: its version ("3.1.0") into out
int  getNewerRelease(char *out, int cap);

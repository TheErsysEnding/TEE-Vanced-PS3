#pragma once

// bench - (V3.1.0 Test 3) playback measured without anyone at the console. A job file, /dev_hdd0/tmp/TEE_Vanced/bench.txt,
// lists videos with a start, a length and a quality; at the next start the app plays them one after the other, writes one
// line of numbers per run to the log and to bench-results.txt beside it, and goes back to the XMB. Without a job file
// nothing changes. See bench.c for the file's format. Without one in /dev_hdd0/tmp the job may lie in the app's data
// folder instead (getAppDataDir): RPCS3 empties /dev_hdd0/tmp whenever it boots a title.

#include "video-player.h"   // VideoPlayer: the play screen's, whose counters the bench reads

// main.c after initAccount: takes the job file if there is one (renamed bench.running at once, so a crash never repeats it)
void initBench(void);
// main.c, every frame before beginGfxFrame: starts, measures and ends the runs
void updateBench(void);
// a job is running: no welcome page, no update check, nothing saved about what was watched
int  isBenchActive(void);
// a global of a job file still waiting (bench.txt), read before initBench - main.c asks for log= before the log writer
// starts. 1 with the value in `value` (cut to cap - 1 bytes), 0 without a job or without that key
int  peekBenchGlobal(const char *key, char *value, int cap);

// ---- the play screen and the bench ----
// A run plays through the app's own play screen, opened as X on a card opens it (playVideo). While that play is the
// bench's, play.c takes the run's settings in place of the user's and saves nothing about it - no resume position, no
// SponsorBlock fetch beside the stream; every other play is untouched.
typedef struct {
   int startSec;     // where the video starts, in place of the resume position
   int qualityCap;   // the highest height to play (0 = auto), in place of getQualityCap - and never saved
   int vp9Spes;      // 5 or 6, in place of getVp9Spes
   int hybrid;       // 0 (nohybrid): a VP9 stream gets no lower stream beside it (play.c's vp9PairCount 0)
   int mute;         // the sound at volume 0; the feed runs on, so the clock the picture keeps to is the same
} BenchPlay;
// 1 with the run's settings while the play screen on top was opened by the bench, 0 for any other play (play left as
// it is). play.c's worker asks too: the settings are set before the screen is pushed and stay until it has gone.
int  getBenchPlay(BenchPlay *play);
int  isBenchPlay(void);
// play.c's fallBackFromVp9 ran: the run ends as "fallback" (play.c rebuilds the player all the same)
void noteBenchVp9Fallback(void);

// what the bench reads of the play screen (play.c): the player on screen - NULL while none is built, or while one is
// being built anew -, whether the play has shown its first picture, and whether it gave up (a reason on screen instead)
VideoPlayer *getPlayScreenPlayer(void);
int  hasPlayScreenPicture(void);
int  hasPlayScreenFailed(void);

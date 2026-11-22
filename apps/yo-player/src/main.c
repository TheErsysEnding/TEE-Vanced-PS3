// yo-player - a native PS3 YouTube client (in progress).
//
// Boilerplate mirrors file-manager (the proven full app): RTC + network up
// front, bridge logging, VSYNC-ON gfx, then the frame loop. It boots into the
// home screen (trending categories); searching / channels / playback stream via
// simple-lib-av. Bring-up diagnostics go to the bridge log (dbg.h).

#include <sys/process.h>

#include "app.h"
#include "vfs.h"
#include "http.h"
#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "storage.h"
#include "app-paths.h"
#include "settings.h"
#include "account.h"
#include "downloads.h"
#include "update-check.h"   // (V3.0.0 Test 12) a newer release on GitHub?
#include "video-export.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/stats.h"
#include "ui-fonts.h"
#include "ui-draw.h"
#include "ui-clock.h"
#include "i18n.h"           // (V3.1.0) the app's texts in the language chosen at the first start
#include "bridge-client.h"
#include "dbg.h"
#include "video-player.h"   // (V3.1.0 Test 3) the display's counters and the player's switches
#include "bench.h"          // ... playback measured from a job file
#include <string.h>

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   appRegisterExitCallback();
   initRtc();
   initVfs();                    // file i/o routing (the temp download lands via openFs)
   migrateAppData();             // create the data dir (and adopt the old /dev_hdd0/tmp one) before anything reads it
   initTheme();                  // palette from themes.txt: every colour below reads activeTheme

   int netRc = initNet();
   initModernHttp();   // bind the modern (BearSSL) http transport; all requests + media streams go through it
   registerWithBridge("app", "yo-player");
   logBuildVersion();   // after registration, so the build line also reaches the bridge client's Logs tab

   // (V3.1.0 Test 4) gfx-share= decides how initGfx sets the command buffer up, and loadSettings runs after initGfx: read
   // here, from settings.txt, a bench job's global gfx-share= winning (one launch with it); rsx-test=small-cb shortens the buffer
   char shareValue[8], rsxTest[16];
   int gfxShare = readEarlySetting("gfx-share", shareValue, sizeof shareValue) && strcmp(shareValue, "on") == 0;
   if (peekBenchGlobal("gfx-share", shareValue, sizeof shareValue)) {
      gfxShare = strcmp(shareValue, "on") == 0;
      logInfo("[yt] gfx-share %s for this launch (the bench job's)\n", gfxShare ? "on" : "off");
   }
   if (!readEarlySetting("rsx-test", rsxTest, sizeof rsxTest)) rsxTest[0] = 0;
   setGfxShareMode(gfxShare, rsxTest);
   if (initGfx(GFX_VSYNC_ON) != 0) return 1;
   // (V3.1.0 Test 3) the player paces display sync by the screen's vblanks and flips, and gives a picture back once the
   // flip after it has happened. (V3.1.0 Test 4) ... and asks the display for TEE VP9's RSX part (gfx.h)
   static const VideoDisplayHooks displayHooks = { getGfxNextFlipId, getGfxFlipsDone, getGfxVblanksMissed, isGfxRsxAllowed,
                                                   isGfxRsxReadable, pauseGfxDrawing, resumeGfxDrawing, brakeGfxRsx,
                                                   setGfxRsxActive };
   setVideoDisplayHooks(&displayHooks);
   if (initAudio() != 0) return 1;
   if (initFont() != 0) return 1;
   if (initIconFont() != 0) logError("[yt] embedded icon font failed to load; icons will be blank\n");
   initUiFonts();          // Manrope's four weights + one shared FONT_POP, opened once for every screen: memory
                           // fonts take user-font slots, and there are only 8 (see ui-fonts.h)
   uiInitClock();          // the header clock's digits, in the 12/24-hour form the XMB is set to (ui-clock.h)
   initPad();

   initStats(5, 5, 14, activeTheme->textSecondary);
   loadSettings();         // user-editable settings.txt (created with defaults on first launch)
   initI18n();             // (V3.1.0) the app's language (language= in the settings; none yet: Home asks first)
   // (V3.1.0 Test 3) the player's switches, and the log on a thread of its own (a job file may ask for log=sync)
   setVideoDisplaySync(getDisplaySyncByFlips());
   setVideoVp9ReleaseEarly(getVp9ReleaseEarly());
   setVideoVp9Rsx(getVp9Rsx());            // (V3.1.0 Test 4) TEE VP9's RSX part - with gfx-share=on only
   char benchRsx[8];                       // ... a bench job's global vp9-rsx= for this launch, as its gfx-share=
   if (peekBenchGlobal("vp9-rsx", benchRsx, sizeof benchRsx)) {
      setVideoVp9Rsx(strcmp(benchRsx, "on") == 0 ? 2 : strcmp(benchRsx, "1440") == 0 ? 1 : 0);
      logInfo("[yt] vp9-rsx %s for this launch (the bench job's)\n", benchRsx);
   }
   setVideoVp9RsxTest(getRsxTest());
   int logAsync = getLogAsync();
   char benchLog[8];
   if (peekBenchGlobal("log", benchLog, sizeof benchLog)) logAsync = strcmp(benchLog, "sync") != 0;
   if (logAsync) startLogWriter();
   initStorage();          // prefs + watch history; seeds the skip categories from the settings above, so
                           // it has to run AFTER loadSettings, not before
   initAccount();          // the YouTube sign-in kept from an earlier start, if any (account.txt)
   initBench();            // (V3.1.0 Test 3) a bench job, if one waits (bench.h): renamed bench.running at once
   loadConsoleGlyphs();    // decode the console's own button glyphs for the on-screen hints
   initUiDraw();           // the Ember helper textures - never before initGfx, whose VRAM setup an earlier
                           // upload breaks
   initVideoExport();      // resolves the download staging folder, so it runs before initDownloads
   initDownloads();        // background download queue + its progress overlay
   // (V3.0.0 Test 12) once a start, on a worker: is a newer release out? (update-check.h) (V3.1.0 Test 3) Not during a
   // bench job: a request beside the runs' streams, and a notice on Home with nobody to read it
   if (!isBenchActive()) startUpdateCheck();

   logInfo("[yt] net rc=%d\n", netRc);
   uiLogVram("startup");   // the baseline the later [vram] lines compare against
   openHome();       // boot into the home screen (trending categories)

   while (!appExitRequested) {
      appPoll();
      updateVideoExport();   // right after the sysutil pump, which is what delivers its results
      updatePad();
      updateScreen();
      updateBench();         // (V3.1.0 Test 3) starts, reads and ends a bench job's runs; nothing without a job

      beginGfxFrame();
      clearGfx(activeTheme->appBg);
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   shutdownDownloads();   // cancel + join any in-flight download before the http/vfs layers go away
   stopUpdateCheck();     // (V3.0.0 Test 12) its one request, likewise
   shutdownVideoExport();
   freeConsoleGlyphs();
   termUiDraw();          // waits for the RSX, then frees the helper textures - while gfx is still up
   termStats();
   termUiFonts();         // the shared fonts: after the download overlay (the last label user) is gone,
                          // before termFont tears the font library down under them
   termAudio();
   termFont();
   termGfx();
   shutdownHttp();
   stopLogWriter();       // (V3.1.0 Test 3) the lines still queued, while the file layer is up
   shutdownVfs();
   return 0;
}

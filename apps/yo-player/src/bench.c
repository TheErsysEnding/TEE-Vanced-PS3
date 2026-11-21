// bench - (V3.1.0 Test 3) playback measured with nobody at the console (see bench.h): a job file names videos, and at
// the next start the app plays them one after the other through its own play screen and writes a line of numbers for
// each, to compare one build or one setting with another overnight.
//
// ==== The job file ====
//
// bench.txt in /dev_hdd0/tmp/TEE_Vanced (DBG_DIR, beside dbg.txt) - or, when there is none there, bench.txt in the app's
// data folder (getAppDataDir, /dev_hdd0/tee-vanced): RPCS3 empties /dev_hdd0/tmp whenever it boots a title, so a job put
// there never reaches the app in the emulator. Plain text, one entry a line. '#' starts a comment, which runs to the end
// of the line; blank lines are ignored, and so is a UTF-8 BOM in front (an editor on Windows writes one).
//
// Globals are key=value lines, anywhere in the file; the last one counts. These are the defaults:
//   start-delay=15   seconds from the start to the first run - Home loads its lists meanwhile
//   warmup=5         seconds of picture before the counting starts
//   pause=5          seconds between two runs
//   timeout=90       seconds a run waits for its first picture before it is "no picture"
//   end=xmb          after the last run: xmb = back to the XMB, stay = Home stays up
//   mute=1           1 = every run plays at volume 0 - the console may stand beside someone asleep. The sound feed runs
//                    on, so the clock the picture keeps to is the one it always is. 0 = at the player's volume
//   log=async        async | sync: how the log is written while the job runs. main.c reads it (peekBenchGlobal) before
//                    the log writer starts; a job without it leaves settings.txt's log= in charge
// The seconds are whole ones, 0 to 3600 (timeout 1 to 3600). An unknown key, or a value its key does not take, is said
// in the log and left at its default.
//
// Every other line is a run, its words apart by spaces or tabs:
//   <videoId> <startSec> <seconds> <quality> [nohybrid] [spes=5|6] [sync=flips|wall] [release=early|late] [r3] [repeat=n]
//   videoId    the video's 11-character id
//   startSec   where it starts, in whole seconds (the player lands on the key frame at or before it)
//   seconds    how many seconds of the video are counted once the warmup is over
//   quality    auto, 720, 1080, 1440 or 2160: this run's quality cap, in place of the one saved (and never saved)
//   nohybrid   VP9 gets no lower stream beside it (play.c's vp9PairCount 0), so it never changes down on a busy stretch
//   spes=      the TEE VP9 building block's SPEs (settings.txt vp9-spes)
//   sync=      display sync by the vblanks missed or by the wall clock (display-sync, setVideoDisplaySync)
//   release=   a VP9 picture goes back at its flip or at the next present (vp9-release, setVideoVp9ReleaseEarly)
//   r3         R3's panel on, as TEE watches while he tests: its numbers are rendered anew every second, on the UI thread
//   repeat=n   the line n times in a row
// spes=, sync= and release= left out are the user's settings, and those are back after every run. At most 32 runs in
// all, repeats counted. A line that does not read as a run - an option it does not know among them - is said in the log
// and left out whole: a run with another setting than the one asked for would measure the wrong thing.
//
// ==== What becomes of it ====
//
// initBench reads the file whole and at once renames it bench.running, beside it: that name is the job's state on the
// disk. A start that finds a bench.running left over (a crash, or a hang the console was switched off for) renames it
// bench.crashed, says so, and runs nothing - a job that took the console down never runs again by itself. Each run adds
// a comment line to bench.running as it opens and as it ends, so bench.crashed tells where it stopped. A job at its end,
// or stopped, becomes bench.done. Someone at the console stops it: X, O or START between runs (from the job's third
// second on - the X that started the app may still be down), or O out of a run's player; the app then stays up. The
// XMB's Quit stops it as well, and is no crash.
//
// ==== A run ====
//
//  1. After start-delay (after a run: pause) the play screen opens on the video as X on a card opens it (playVideo,
//     over Home, whose suspend stops its fetches) - but from startSec, with the run's quality cap, SPEs and lower
//     stream, at volume 0 with mute=1, and saving nothing: no resume position, no history entry, no quality cap; and
//     no request beside the stream's (no SponsorBlock fetch, no /next for the chapters). play.c asks getBenchPlay.
//  2. The first picture within timeout seconds, else "no picture". warmup seconds after it the player's counters
//     (getVideoPlayerCounters) and the screen's (getGfxFlipStats, getGfxVblanksMissed) are the baseline.
//  3. Read every frame until `seconds` of the video have played since ("ok"), the video ended ("ended"), its decoder
//     stopped for good with nothing to take over ("failed"), play.c went over from VP9 to H.264 ("fallback":
//     fallBackFromVp9 - play.c goes on as always, the run ends there), or seconds + 60 went by without any of these
//     ("stuck"). A player built anew during a run (a quality change, reopenAt) adds to what the one before it counted.
//  4. One line of numbers to the log and to bench-results.txt in DBG_DIR, wherever the job came from; the play screen
//     closed, the user's switches back. The next frame the memory left is logged.
//
// ==== The line ====
//
// [bench] run 3/8 tqE50cUXGCs 1021+60 q1440 hybrid sync=flips release=early spes=6: ok at 1086 s in 60.1 s
//   | 2560x1440 59940 mHz vp9 | presents 3600 shown 3590 late 12 repeats 8 starved 3
//   | flips 3596 missed 3 gaps25 3 gaps42 0 maxgap 41 ms | dec 643 frames 3610 pictures 3598 leftout 38 rsx 0 waits 0+0
//   | holds 1 240 ms | down 2 up 2 safety 0 restarts 0 | mem 18 MB
// - one line, integers only: the app's printf has no floating point. After the run itself: what it ended in, where the
// video was then and how long the counting took by the clock; the stream (the last player's), its frame rate in mHz;
// the player's presents, those with a new picture, the pictures dropped as late, the presents without a new one and
// those with none ready at all; the screen's flips, the vblanks it showed a frame again for, the gaps over 25 and over
// 41.7 ms and the longest (*); TEE VP9's pictures a second while busy (times 10, at the end), its frames, pictures,
// frames left out, RSX frames and entropy + records waits; the sound's holds for the picture and how long they took;
// the changes to the lower stream and back, the safety net's words, TEE VP9 opened anew; the user memory free once the
// decoder was open. The stream, dec and mem are the player's at the end, everything else counts the window from the
// baseline to the end.
// (*) gfx keeps one longest gap since initGfx, which a start or a run before may hold for good. When the window set no
//     new one, its own longest frame on the UI thread (updateBench to updateBench, which the flips follow with VSYNC on)
//     stands in for it.
// A run that ended before the counting says only how: "no picture after 90.0 s", "fallback before the first picture,
// after 12.3 s", "ended at 1090 s, in the warmup".

#include "bench.h"
#include "screens/play.h"       // playVideo, playScreen
#include "screen-manager.h"     // popScreen
#include "settings.h"           // the user's switches, back after every run
#include "app-paths.h"          // getAppDataDir, APP_VERSION
#include "app.h"                // requestAppExit, appExitRequested
#include "pad.h"
#include "gfx.h"                // getGfxFlipStats, getGfxVblanksMissed, the VRAM in use
#include "vfs.h"
#include "dbg.h"
#include <sys/sys_time.h>       // sys_time_get_system_time
#include <sys/memory.h>         // sys_memory_get_user_memory_size
#include <cell/rtc.h>           // the date on the job's first line
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BUILD_STAMP
#define BUILD_STAMP "?"
#endif

#define JOB_NAME           "bench.txt"
#define RUNNING_NAME       "bench.running"
#define CRASHED_NAME       "bench.crashed"
#define DONE_NAME          "bench.done"
#define RESULTS_PATH       DBG_DIR "/bench-results.txt"
#define PROGRESS_MARK      "# (bench) "        // the lines a job's runs add to bench.running
#define PATH_CAP           128
#define JOB_MAX_BYTES      (32 * 1024)         // 32 runs and their comments take a few KB
#define LEFTOVER_MAX_BYTES (64 * 1024)         // ... a crashed one, its progress lines after them
#define MAX_RUNS           32
#define RUN_MAX_WORDS      12
#define GLOBAL_MAX_S       3600
#define STUCK_EXTRA_S      60
#define SCREEN_POPS_MAX    8                   // screen-manager.c's stack
#define SECOND_US          1000000ull
#define PAD_GRACE_US       (2 * SECOND_US)     // the job's first seconds: the X that started the app may still be down
#define FAILED_GRACE_US    SECOND_US           // a decoder that stopped: play.c's time to go over to H.264 first

enum { RESULT_OK, RESULT_ENDED, RESULT_FAILED, RESULT_FALLBACK, RESULT_STUCK, RESULT_NO_PICTURE, RESULT_COUNT };
static const char *const RESULT_NAMES[RESULT_COUNT] = { "ok", "ended", "failed", "fallback", "stuck", "no picture" };

// the player's counters that count up (VideoPlayerCounters), as one array: what the window adds of each
enum { C_PRESENTS, C_SHOWN, C_LATE, C_REPEATS, C_STARVED, C_HOLDS, C_HOLD_MS, C_TO_LOW, C_TO_HIGH, C_SAFETY, C_RESTARTS,
       C_FRAMES, C_PICTURES, C_LEFT_OUT, C_RSX, C_ENTROPY_WAITS, C_RECORDS_WAITS, C_COUNT };

typedef struct {
   char id[12];
   int  startSec, seconds;
   int  quality;   // the cap: 0 auto, else a height
   int  hybrid;    // 0: nohybrid
   int  spes;      // 5 or 6; 0: the user's (vp9-spes)
   int  sync;      // 1 flips, 0 wall; -1: the user's (display-sync)
   int  release;   // 1 early, 0 late; -1: the user's (vp9-release)
   int  r3;        // R3's panel on
} BenchRun;

typedef enum { BENCH_OFF, BENCH_WAITING, BENCH_OPENING, BENCH_WARMUP, BENCH_COUNTING } BenchStage;

// the run on - all of it 0 as each run starts
typedef struct {
   uint64_t            startUs;          // the play screen pushed
   uint64_t            pictureUs;        // its first picture
   uint64_t            countStartUs;     // the baseline
   uint64_t            failedSinceUs;    // the player has said it failed, since then (0: it has not)
   uint64_t            lastFrameUs;      // the counting's last frame on the UI thread ...
   uint64_t            frameMaxUs;       // ... and its longest (maxgap, see the top)
   int                 fallback;         // fallBackFromVp9 ran
   VideoPlayer        *player;           // the one read last frame (NULL: none, or one being built anew)
   VideoPlayerCounters last;             // its counters then: the stream, dec and mem come from here
   int                 lastVp9;
   float               lastPos, basePos; // where the video was last frame, and at the baseline
   uint32_t            seen[C_COUNT];    // the player's counters, last frame
   uint32_t            base[C_COUNT];    // ... what of them lies before the window (0 for a player built in it)
   uint32_t            sums[C_COUNT];    // what the players gone added in the window
   GfxFlipStats        flips;            // the screen's at the baseline
   uint32_t            missed;           // getGfxVblanksMissed then
} BenchRunState;

static struct {
   BenchStage    stage;
   char          dir[64];             // the job's folder: bench.running and the rest beside its bench.txt
   int           startDelay, warmup, pause, timeout, endXmb, mute;
   int           logAsync;            // log=: 1 async, 0 sync, -1 not in the job
   BenchRun      runs[MAX_RUNS];
   int           runCount;
   int           current;             // the run on, or the next one
   int           tally[RESULT_COUNT];
   uint64_t      jobStartUs;          // the job's first frame
   uint64_t      nextRunUs;
   int           memoryDue, memoryRun;   // the run whose memory the next update logs
   BenchPlay     play;                // what play.c is told while ...
   int           playOwned;           // ... the play screen is the run's
   BenchRunState on;
} bench;

static uint64_t nowUs(void) { return (uint64_t)sys_time_get_system_time(); }

// a time as seconds and tenths, for a printf without floating point
static void splitTenths(uint64_t us, unsigned *whole, unsigned *tenth)
{
   uint64_t tenths = (us + 50000ull) / 100000ull;
   *whole = (unsigned)(tenths / 10);
   *tenth = (unsigned)(tenths % 10);
}

static int addText(char *out, int cap, int used, const char *format, ...) __attribute__((format(printf, 4, 5)));

// text after the `used` bytes already in out, never past its end: the length then
static int addText(char *out, int cap, int used, const char *format, ...)
{
   if (used < 0 || used >= cap - 1) return used;
   va_list args;
   va_start(args, format);
   int n = vsnprintf(out + used, (size_t)(cap - used), format, args);
   va_end(args);
   if (n < 0) return used;
   return used + n < cap - 1 ? used + n : cap - 1;
}

// ---- files ----

static void jobPath(char *out, const char *dir, const char *name) { snprintf(out, PATH_CAP, "%s/%s", dir, name); }

static int jobFileExists(const char *dir, const char *name)
{
   char path[PATH_CAP];
   jobPath(path, dir, name);
   return fileExists(path);
}

// where a job may lie: DBG_DIR, then the data folder (RPCS3 empties /dev_hdd0/tmp at every boot). 1 or 2 of them
static int jobFolders(const char *folders[2])
{
   folders[0] = DBG_DIR;
   folders[1] = getAppDataDir();
   return strcmp(folders[0], folders[1]) == 0 ? 1 : 2;
}

// a file whole and NUL-terminated, at most `max` bytes of it (free it); NULL when it is not there or cannot be read.
// *cut: it was longer.
static char *readWhole(const char *path, int max, int *cut)
{
   VfsStat info;
   if (statPath(path, &info) != 0 || info.isDir) return NULL;
   int size = info.size > (uint64_t)max ? max : (int)info.size;
   char *text = (char *)malloc((size_t)size + 1);
   if (!text) return NULL;
   if (readFile(path, text, size + 1) < 0) { free(text); return NULL; }
   if (cut) *cut = info.size > (uint64_t)size;
   return text;
}

// a line onto the end of a file, made when it is not there (and its folder with it, when `folder` is given: the
// console clears /dev_hdd0/tmp now and then, as logEmit knows)
static void appendLine(const char *path, const char *folder, const char *text)
{
   VfsFile file;
   int opened = openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &file) == 0;
   if (!opened && folder && makeDir(folder) == 0)
      opened = openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &file) == 0;
   if (!opened) return;
   size_t length = strlen(text);
   if (length) writeFs(&file, text, (uint64_t)length);
   writeFs(&file, "\n", 1);
   closeFs(&file);
}

// a line of bench-results.txt: always in DBG_DIR beside the log, stamped with the time the log's lines have
static void addResult(const char *text)
{
   char line[1024];
   int stamp = dbgFormatTimestamp(line);   // "[HH:MM:SS.mmm] ", without a NUL
   snprintf(line + stamp, sizeof line - (size_t)stamp, "%s", text);
   appendLine(RESULTS_PATH, DBG_DIR, line);
}

// a comment line in bench.running: where the job is, for bench.crashed to tell should it never come to its end
static void noteProgress(const char *text)
{
   char path[PATH_CAP], line[320];
   jobPath(path, bench.dir, RUNNING_NAME);
   int stamp = dbgFormatTimestamp(line);
   snprintf(line + stamp, sizeof line - (size_t)stamp, "%s", text);
   char marked[340];
   snprintf(marked, sizeof marked, PROGRESS_MARK "%s", line);
   appendLine(path, NULL, marked);
}

// `from` to the name `to` beside it; failing that, `from` is deleted. 1 renamed (or nothing to move), 0 deleted, -1
// still there. Whatever has the name `to` goes first: cellFs's rename fails onto a file that is there, and a
// bench.done of an earlier job would leave bench.running behind - the next job would be taken for one that crashed.
static int moveAside(const char *from, const char *to)
{
   if (!fileExists(from)) return 1;
   deleteFile(to);
   if (renamePath(from, to) == 0) return 1;
   return deleteFile(from) == 0 ? 0 : -1;
}

// ---- the job file's lines ----

static char *skipBom(char *text)
{
   const unsigned char *bytes = (const unsigned char *)text;
   return bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? text + 3 : text;
}

// the next line of the text, its comment cut off and its ends trimmed ("" for a blank one); NULL after the last one.
// Cuts the text in place.
static char *nextLine(char **cursor, int *number)
{
   char *line = *cursor;
   if (!*line) return NULL;
   char *end = strchr(line, '\n');
   if (end) { *end = 0; *cursor = end + 1; }
   else     *cursor = line + strlen(line);
   (*number)++;
   char *hash = strchr(line, '#');
   if (hash) *hash = 0;
   while (*line == ' ' || *line == '\t') line++;
   size_t length = strlen(line);
   while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r')) line[--length] = 0;
   return line;
}

// key=value with nothing but the key before the '=': 1 with both cut out of the line. A run's line has words before
// its first '=' (an option such as sync=wall), so it is never taken for one.
static int splitGlobal(char *line, char **key, char **value)
{
   char *equals = strchr(line, '=');
   if (!equals) return 0;
   char *keyEnd = equals;
   while (keyEnd > line && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) keyEnd--;
   if (keyEnd == line) return 0;
   for (const char *c = line; c < keyEnd; c++) if (*c == ' ' || *c == '\t') return 0;
   *keyEnd = 0;
   char *at = equals + 1;
   while (*at == ' ' || *at == '\t') at++;
   *key = line;
   *value = at;
   return 1;
}

// a whole number of at most 6 digits and nothing else: 1 with it in *value
static int readNumber(const char *text, int *value)
{
   int number = 0, digits = 0;
   for (; *text >= '0' && *text <= '9'; text++) {
      if (++digits > 6) return 0;
      number = number * 10 + (*text - '0');
   }
   if (!digits || *text) return 0;
   *value = number;
   return 1;
}

static int isVideoId(const char *text)
{
   int length = 0;
   for (; text[length]; length++) {
      char c = text[length];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
   }
   return length == 11;
}

static void readSeconds(int number, const char *key, const char *value, int *target, int least)
{
   int seconds;
   if (readNumber(value, &seconds) && seconds >= least && seconds <= GLOBAL_MAX_S) *target = seconds;
   else logWarn("[bench] line %d: %s takes whole seconds from %d to %d, not \"%.24s\" - left at %d\n", number, key, least,
                GLOBAL_MAX_S, value, *target);
}

static void readGlobal(int number, const char *key, const char *value)
{
   if      (strcmp(key, "start-delay") == 0) readSeconds(number, key, value, &bench.startDelay, 0);
   else if (strcmp(key, "warmup") == 0)      readSeconds(number, key, value, &bench.warmup, 0);
   else if (strcmp(key, "pause") == 0)       readSeconds(number, key, value, &bench.pause, 0);
   else if (strcmp(key, "timeout") == 0)     readSeconds(number, key, value, &bench.timeout, 1);
   else if (strcmp(key, "end") == 0) {
      if      (strcmp(value, "xmb") == 0)  bench.endXmb = 1;
      else if (strcmp(value, "stay") == 0) bench.endXmb = 0;
      else logWarn("[bench] line %d: end takes xmb or stay, not \"%.24s\" - left at %s\n", number, value,
                   bench.endXmb ? "xmb" : "stay");
   } else if (strcmp(key, "mute") == 0) {
      if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) bench.mute = value[0] - '0';
      else logWarn("[bench] line %d: mute takes 0 or 1, not \"%.24s\" - left at %d\n", number, value, bench.mute);
   } else if (strcmp(key, "log") == 0) {
      // as main.c takes it: anything but sync is async
      bench.logAsync = strcmp(value, "sync") != 0;
      if (bench.logAsync && strcmp(value, "async") != 0)
         logWarn("[bench] line %d: log takes async or sync, not \"%.24s\" - async\n", number, value);
   } else {
      logWarn("[bench] line %d: \"%.24s\" is no key of a job - ignored\n", number, key);
   }
}

// a run's line: one run into the job, or `repeat` of them; those past MAX_RUNS are counted into *dropped
static void readRun(int number, char *line, int *dropped)
{
   char *words[RUN_MAX_WORDS];
   int count = 0;
   for (char *at = line; *at; ) {
      if (*at == ' ' || *at == '\t') { at++; continue; }
      if (count == RUN_MAX_WORDS) { logWarn("[bench] line %d: more words than a run has - left out\n", number); return; }
      words[count++] = at;
      while (*at && *at != ' ' && *at != '\t') at++;
      if (*at) *at++ = 0;
   }
   if (count < 4) {
      logWarn("[bench] line %d: a run is <videoId> <startSec> <seconds> <quality> [options] - left out\n", number);
      return;
   }
   BenchRun run;
   memset(&run, 0, sizeof run);
   run.hybrid = 1;
   run.sync = run.release = -1;
   int repeat = 1;
   if (!isVideoId(words[0])) {
      logWarn("[bench] line %d: \"%.24s\" is no video id (11 letters, digits, - or _) - left out\n", number, words[0]);
      return;
   }
   memcpy(run.id, words[0], sizeof run.id);   // its 11 characters and the NUL
   if (!readNumber(words[1], &run.startSec)) {
      logWarn("[bench] line %d: the start is whole seconds, not \"%.24s\" - left out\n", number, words[1]);
      return;
   }
   if (!readNumber(words[2], &run.seconds) || run.seconds <= 0) {
      logWarn("[bench] line %d: the length is whole seconds above 0, not \"%.24s\" - left out\n", number, words[2]);
      return;
   }
   if (strcmp(words[3], "auto") == 0) run.quality = 0;
   else if (!readNumber(words[3], &run.quality) ||
            (run.quality != 720 && run.quality != 1080 && run.quality != 1440 && run.quality != 2160)) {
      logWarn("[bench] line %d: the quality is auto, 720, 1080, 1440 or 2160, not \"%.24s\" - left out\n", number, words[3]);
      return;
   }
   for (int i = 4; i < count; i++) {
      const char *option = words[i];
      if      (strcmp(option, "nohybrid") == 0)      run.hybrid = 0;
      else if (strcmp(option, "spes=5") == 0)        run.spes = 5;
      else if (strcmp(option, "spes=6") == 0)        run.spes = 6;
      else if (strcmp(option, "sync=flips") == 0)    run.sync = 1;
      else if (strcmp(option, "sync=wall") == 0)     run.sync = 0;
      else if (strcmp(option, "release=early") == 0) run.release = 1;
      else if (strcmp(option, "release=late") == 0)  run.release = 0;
      else if (strcmp(option, "r3") == 0)            run.r3 = 1;
      else if (strncmp(option, "repeat=", 7) == 0 && readNumber(option + 7, &repeat) && repeat >= 1 && repeat <= MAX_RUNS) {
         // the number is in
      } else {
         logWarn("[bench] line %d: \"%.24s\" is no run option (nohybrid, spes=5|6, sync=flips|wall, release=early|late, r3, "
                 "repeat=1..%d) - left out\n", number, option, MAX_RUNS);
         return;
      }
   }
   for (int i = 0; i < repeat; i++) {
      if (bench.runCount < MAX_RUNS) bench.runs[bench.runCount++] = run;
      else (*dropped)++;
   }
}

static void parseJob(char *text)
{
   bench.startDelay = 15;
   bench.warmup = 5;
   bench.pause = 5;
   bench.timeout = 90;
   bench.endXmb = 1;
   bench.mute = 1;
   bench.logAsync = -1;
   int number = 0, dropped = 0;
   char *cursor = skipBom(text);
   for (char *line; (line = nextLine(&cursor, &number)) != NULL; ) {
      if (!*line) continue;
      char *key, *value;
      if (splitGlobal(line, &key, &value)) readGlobal(number, key, value);
      else readRun(number, line, &dropped);
   }
   if (dropped) logWarn("[bench] %d run%s past the %d a job may have - left out\n", dropped, dropped == 1 ? "" : "s", MAX_RUNS);
}

// ---- a run's words ----

static int runSync(const BenchRun *run)    { return run->sync >= 0 ? run->sync : getDisplaySyncByFlips(); }
static int runRelease(const BenchRun *run) { return run->release >= 0 ? run->release : getVp9ReleaseEarly(); }
static int runSpes(const BenchRun *run)    { return run->spes ? run->spes : getVp9Spes(); }

// "run 3/8 tqE50cUXGCs 1021+60 q1440 hybrid sync=flips release=early spes=6": how every line about a run begins
static void describeRun(char *out, int cap, int index)
{
   const BenchRun *run = &bench.runs[index];
   char quality[8];
   if (run->quality) snprintf(quality, sizeof quality, "%d", run->quality);
   else              strcpy(quality, "auto");
   snprintf(out, (size_t)cap, "run %d/%d %s %d+%d q%s %s sync=%s release=%s spes=%d%s", index + 1, bench.runCount, run->id,
            run->startSec, run->seconds, quality, run->hybrid ? "hybrid" : "nohybrid", runSync(run) ? "flips" : "wall",
            runRelease(run) ? "early" : "late", runSpes(run), run->r3 ? " r3" : "");
}

// the user's switches, as settings.txt has them: after every run, and when a job stops
static void restoreSwitches(void)
{
   setVideoDisplaySync(getDisplaySyncByFlips());
   setVideoVp9ReleaseEarly(getVp9ReleaseEarly());
   setVideoVp9Spes(getVp9Spes());
}

// ---- the job's start and end ----

// the job's first line, in the log and in bench-results.txt: what it is, on what build, with what around it
static void announceJob(const char *path)
{
   CellRtcDateTime date;
   memset(&date, 0, sizeof date);
   cellRtcGetCurrentClockLocalTime(&date);
   int logAsync = bench.logAsync >= 0 ? bench.logAsync : getLogAsync();
   char line[600];
   snprintf(line, sizeof line, "[bench] job %s, %04d-%02d-%02d %02d:%02d, TEE Vanced %s build %s: %d run%s | start-delay %d s, "
            "warmup %d s, pause %d s, timeout %d s, end %s, mute %d, log %s | settings.txt display-sync %s, vp9-release %s, "
            "vp9-spes %d", path, (int)date.year, (int)date.month, (int)date.day, (int)date.hour, (int)date.minute, APP_VERSION,
            BUILD_STAMP, bench.runCount, bench.runCount == 1 ? "" : "s", bench.startDelay, bench.warmup, bench.pause,
            bench.timeout, bench.endXmb ? "xmb" : "stay", bench.mute, logAsync ? "async" : "sync",
            getDisplaySyncByFlips() ? "flips" : "wall", getVp9ReleaseEarly() ? "early" : "late", getVp9Spes());
   logInfo("%s\n", line);
   appendLine(RESULTS_PATH, DBG_DIR, "");   // a blank line between two jobs, for the eye
   addResult(line);
}

// bench.running -> bench.done: the job is over, and the next start must not take it for one that crashed
static void retireJob(void)
{
   char running[PATH_CAP], done[PATH_CAP];
   jobPath(running, bench.dir, RUNNING_NAME);
   jobPath(done, bench.dir, DONE_NAME);
   int moved = moveAside(running, done);
   if (moved == 0) logWarn("[bench] " RUNNING_NAME " could not be renamed " DONE_NAME " and is deleted\n");
   else if (moved < 0)
      logError("[bench] " RUNNING_NAME " could neither be renamed " DONE_NAME " nor deleted: the next start takes the "
               "job for one that crashed\n");
}

static void finishJob(void)
{
   char line[200];
   int used = addText(line, sizeof line, 0, "[bench] done: %d run%s", bench.runCount, bench.runCount == 1 ? "" : "s");
   const char *join = " - ";
   for (int r = 0; r < RESULT_COUNT; r++) {
      if (!bench.tally[r]) continue;
      used = addText(line, sizeof line, used, "%s%d %s", join, bench.tally[r], RESULT_NAMES[r]);
      join = ", ";
   }
   logInfo("%s\n", line);
   addResult(line);
   retireJob();
   bench.stage = BENCH_OFF;
   // a job with no run in it leaves the app up, so its log lines can be read on the spot
   if (bench.endXmb && bench.runCount > 0) {
      logInfo("[bench] end=xmb: back to the XMB\n");
      requestAppExit();   // the main loop's way out, as the XMB's Quit
   }
}

// stopped before its end: someone took the pad, or the app is closing. The app stays as it is.
static void stopJob(const char *how)
{
   int inRun = bench.stage >= BENCH_OPENING;
   // a player still up means the app is closing with it (main.c's changeScreen(NULL) ends it): it stays the run's, so
   // its termPlay saves nothing either
   if (playScreen.status == SCREEN_TERMINATED) bench.playOwned = 0;
   restoreSwitches();
   char line[200];
   if (bench.current >= bench.runCount)
      snprintf(line, sizeof line, "[bench] %s, after the last run", how);
   else
      snprintf(line, sizeof line, "[bench] %s, %s run %d/%d", how, inRun ? "in" : "before", bench.current + 1, bench.runCount);
   logInfo("%s\n", line);
   addResult(line);
   retireJob();
   bench.stage = BENCH_OFF;
}

// a bench.running from a start before: that job never came to its end - a crash, or the power. It becomes
// bench.crashed and nothing runs this start, so a job that took the console down never runs again by itself.
static void takeLeftover(const char *dir)
{
   char running[PATH_CAP], crashed[PATH_CAP], where[240] = "";
   jobPath(running, dir, RUNNING_NAME);
   jobPath(crashed, dir, CRASHED_NAME);
   char *text = readWhole(running, LEFTOVER_MAX_BYTES, NULL);
   if (text) {   // the last line a run added: where it stopped
      const char *last = NULL;
      for (const char *at = strstr(text, PROGRESS_MARK); at; at = strstr(at + 1, PROGRESS_MARK)) last = at;
      if (last) {
         last += strlen(PROGRESS_MARK);
         size_t length = strcspn(last, "\r\n");
         if (length > sizeof where - 1) length = sizeof where - 1;
         memcpy(where, last, length);
         where[length] = 0;
      }
      free(text);
   }
   int moved = moveAside(running, crashed);
   char line[480];
   snprintf(line, sizeof line, "[bench] the job in %s did not come to its end (last: %s) - %s; nothing runs this start", dir,
            where[0] ? where : "before its first run",
            moved > 0 ? "it is " CRASHED_NAME " now" : moved == 0 ? RUNNING_NAME " is deleted, " CRASHED_NAME " could not be made"
                      : RUNNING_NAME " could neither be renamed nor deleted, and no job runs while it is there");
   logWarn("%s\n", line);
   addResult(line);
}

// the job in `dir`: read, renamed bench.running at once, its runs ready for updateBench
static void startJob(const char *dir)
{
   char path[PATH_CAP], running[PATH_CAP];
   jobPath(path, dir, JOB_NAME);
   jobPath(running, dir, RUNNING_NAME);
   int cut = 0;
   char *text = readWhole(path, JOB_MAX_BYTES, &cut);
   if (!text) { logWarn("[bench] %s could not be read - no job this start\n", path); return; }
   size_t length = strlen(text);
   int endsLine = length == 0 || text[length - 1] == '\n';
   // renamed before anything else happens: from here on a crash leaves bench.running behind, never the job itself
   int moved = moveAside(path, running);
   if (moved < 0) {
      logError("[bench] %s could neither be renamed nor deleted - not run, or it would run at every start\n", path);
      free(text);
      return;
   }
   if (moved == 0) logWarn("[bench] %s could not be renamed " RUNNING_NAME ", only deleted: it runs this once\n", path);
   if (moved > 0 && !endsLine) appendLine(running, NULL, "");   // the runs' progress lines on lines of their own
   if (cut) logWarn("[bench] %s is longer than %d KB: only its first %d KB are read\n", path, JOB_MAX_BYTES / 1024,
                    JOB_MAX_BYTES / 1024);
   snprintf(bench.dir, sizeof bench.dir, "%s", dir);
   parseJob(text);
   free(text);
   announceJob(path);
   if (bench.runCount == 0) {
      logWarn("[bench] the job has no run in it\n");
      finishJob();
      return;
   }
   bench.stage = BENCH_WAITING;   // start-delay counts from the first frame (updateBench)
}

// ---- a run ----

static void endRun(int result, uint64_t now);

static void logMemory(int run)
{
   sys_memory_info_t memory;
   unsigned freeMb = 0, totalMb = 0;
   if (sys_memory_get_user_memory_size(&memory) == CELL_OK) {
      freeMb = (unsigned)(memory.available_user_memory >> 20);
      totalMb = (unsigned)(memory.total_user_memory >> 20);
   }
   logInfo("[bench] after run %d/%d: user memory %u MB free of %u MB, VRAM %u KB used, %u KB free\n", run + 1,
           bench.runCount, freeMb, totalMb, (unsigned)(getUsedVram() / 1024u), (unsigned)(getFreeVram() / 1024u));
}

// the play screen goes, and with it the run's hold on play.c
static void closeRun(void)
{
   for (int i = 0; i < SCREEN_POPS_MAX && playScreen.status != SCREEN_TERMINATED; i++) popScreen();   // what is over it too
   bench.playOwned = 0;
   bench.on.player = NULL;
   restoreSwitches();
}

static void startRun(uint64_t now)
{
   const BenchRun *run = &bench.runs[bench.current];
   memset(&bench.on, 0, sizeof bench.on);
   bench.on.startUs = now;
   bench.play.startSec   = run->startSec;
   bench.play.qualityCap = run->quality;
   bench.play.vp9Spes    = runSpes(run);
   bench.play.hybrid     = run->hybrid;
   bench.play.showStats  = run->r3;
   bench.play.mute       = bench.mute;
   // the process-wide switches the player reads, before it is built (play.c's worker takes the SPEs from getBenchPlay)
   setVideoDisplaySync(runSync(run));
   setVideoVp9ReleaseEarly(runRelease(run));
   setVideoVp9Spes(bench.play.vp9Spes);

   char what[160], note[200];
   describeRun(what, sizeof what, bench.current);
   logInfo("[bench] %s: opening\n", what);
   snprintf(note, sizeof note, "%s: opened", what);
   noteProgress(note);

   // the run's before the push: initPlay asks at once (its SponsorBlock fetch), the worker it starts right after
   bench.playOwned = 1;
   bench.stage = BENCH_OPENING;
   playVideo(run->id);   // as X on a card: over Home, whose suspend stops its fetches for the stream
   if (playScreen.status == SCREEN_TERMINATED) {   // the screen stack was full: nothing opened, nothing to wait for
      logError("[bench] the play screen did not open\n");
      endRun(RESULT_NO_PICTURE, now);
   }
}

// the play screen's player, read every frame: a new one (a quality change, reopenAt) or none adds what the last one
// counted to the sums, so what was counted before a rebuild stays counted
static void sampleRun(void)
{
   BenchRunState *on = &bench.on;
   VideoPlayer *player = getPlayScreenPlayer();
   if (player != on->player) {
      for (int i = 0; i < C_COUNT; i++) {
         on->sums[i] += on->seen[i] - on->base[i];
         on->base[i] = on->seen[i] = 0;
      }
      on->player = player;
   }
   if (!player) return;
   VideoPlayerCounters counters;
   getVideoPlayerCounters(player, &counters);
   uint32_t now[C_COUNT];
   now[C_PRESENTS] = counters.presents;
   now[C_SHOWN] = counters.shown;
   now[C_LATE] = counters.late;
   now[C_REPEATS] = counters.repeats;
   now[C_STARVED] = counters.starved;
   now[C_HOLDS] = counters.holds;
   now[C_HOLD_MS] = counters.holdMs;
   now[C_TO_LOW] = counters.toLow;
   now[C_TO_HIGH] = counters.toHigh;
   now[C_SAFETY] = counters.safetyNets;
   now[C_RESTARTS] = counters.restarts;
   now[C_FRAMES] = counters.vp9Frames > 0 ? (uint32_t)counters.vp9Frames : 0;
   now[C_PICTURES] = counters.vp9Pictures > 0 ? (uint32_t)counters.vp9Pictures : 0;
   now[C_LEFT_OUT] = counters.vp9Skipped > 0 ? (uint32_t)counters.vp9Skipped : 0;
   now[C_RSX] = counters.vp9RsxFrames > 0 ? (uint32_t)counters.vp9RsxFrames : 0;
   now[C_ENTROPY_WAITS] = counters.vp9EntropyWaits > 0 ? (uint32_t)counters.vp9EntropyWaits : 0;
   now[C_RECORDS_WAITS] = counters.vp9RecordsWaits > 0 ? (uint32_t)counters.vp9RecordsWaits : 0;
   for (int i = 0; i < C_COUNT; i++) {
      // a number that went back has started again (TEE VP9 opened anew, a new player where the old one was): the same
      if (now[i] < on->seen[i]) {
         on->sums[i] += on->seen[i] - on->base[i];
         on->base[i] = 0;
      }
      on->seen[i] = now[i];
   }
   on->last = counters;
   on->lastVp9 = isVideoVp9(player);
   on->lastPos = getVideoPositionSeconds(player);
}

static void startCounting(uint64_t now)
{
   BenchRunState *on = &bench.on;
   memcpy(on->base, on->seen, sizeof on->base);
   memset(on->sums, 0, sizeof on->sums);
   getGfxFlipStats(&on->flips);
   on->missed = getGfxVblanksMissed();
   on->basePos = on->lastPos;
   on->countStartUs = on->lastFrameUs = now;
   on->frameMaxUs = 0;
   bench.stage = BENCH_COUNTING;
   logInfo("[bench] run %d/%d: counting from %d s\n", bench.current + 1, bench.runCount, (int)on->basePos);
}

static void endRun(int result, uint64_t now)
{
   BenchRunState *on = &bench.on;
   char what[160];
   describeRun(what, sizeof what, bench.current);
   char line[900];
   int used = addText(line, sizeof line, 0, "[bench] %s: %s", what, RESULT_NAMES[result]);
   unsigned whole, tenth;
   if (bench.stage == BENCH_OPENING) {   // no picture yet: how long it was waited for
      splitTenths(now - on->startUs, &whole, &tenth);
      if (result == RESULT_NO_PICTURE) used = addText(line, sizeof line, used, " after %u.%u s", whole, tenth);
      else used = addText(line, sizeof line, used, " before the first picture, after %u.%u s", whole, tenth);
   } else if (bench.stage == BENCH_WARMUP) {
      used = addText(line, sizeof line, used, " at %d s, in the warmup", (int)on->lastPos);
   } else {
      GfxFlipStats flips;
      getGfxFlipStats(&flips);
      uint32_t missed = getGfxVblanksMissed() - on->missed;
      uint32_t total[C_COUNT];
      for (int i = 0; i < C_COUNT; i++) total[i] = on->sums[i] + on->seen[i] - on->base[i];
      // gfx's longest gap is the longest since initGfx: one longer than at the baseline came in the window, else the
      // window's own longest frame stands in (see the top)
      uint64_t gapUs = flips.maxGapUs > on->flips.maxGapUs ? (uint64_t)flips.maxGapUs
                     : on->frameMaxUs < (uint64_t)on->flips.maxGapUs ? on->frameMaxUs : (uint64_t)on->flips.maxGapUs;
      const VideoPlayerCounters *c = &on->last;
      splitTenths(now - on->countStartUs, &whole, &tenth);
      used = addText(line, sizeof line, used, " at %d s in %u.%u s | %dx%d %d mHz %s", (int)on->lastPos, whole, tenth,
                     c->width, c->height, c->fpsMilli, on->lastVp9 ? "vp9" : "h264");
      used = addText(line, sizeof line, used, " | presents %u shown %u late %u repeats %u starved %u",
                     (unsigned)total[C_PRESENTS], (unsigned)total[C_SHOWN], (unsigned)total[C_LATE],
                     (unsigned)total[C_REPEATS], (unsigned)total[C_STARVED]);
      used = addText(line, sizeof line, used, " | flips %u missed %u gaps25 %u gaps42 %u maxgap %u ms",
                     (unsigned)(flips.flips - on->flips.flips), (unsigned)missed, (unsigned)(flips.gaps25 - on->flips.gaps25),
                     (unsigned)(flips.gaps42 - on->flips.gaps42), (unsigned)((gapUs + 500) / 1000));
      used = addText(line, sizeof line, used, " | dec %d frames %u pictures %u leftout %u rsx %u waits %u+%u",
                     c->vp9BusyFpsX10, (unsigned)total[C_FRAMES], (unsigned)total[C_PICTURES], (unsigned)total[C_LEFT_OUT],
                     (unsigned)total[C_RSX], (unsigned)total[C_ENTROPY_WAITS], (unsigned)total[C_RECORDS_WAITS]);
      used = addText(line, sizeof line, used, " | holds %u %u ms | down %u up %u safety %u restarts %u | mem %d MB",
                     (unsigned)total[C_HOLDS], (unsigned)total[C_HOLD_MS], (unsigned)total[C_TO_LOW],
                     (unsigned)total[C_TO_HIGH], (unsigned)total[C_SAFETY], (unsigned)total[C_RESTARTS], c->memFreeMb);
   }
   logInfo("%s\n", line);
   addResult(line);
   bench.tally[result]++;
   char note[200];
   snprintf(note, sizeof note, "%s: %s", what, RESULT_NAMES[result]);
   noteProgress(note);   // a crash on the way to the next run then says this one was over

   // over before the screen goes - the flag for updateRun's check, which takes a player closed during a run for the
   // pad's O and looks at nothing else
   bench.stage = BENCH_WAITING;
   closeRun();
   bench.memoryDue = 1;
   bench.memoryRun = bench.current;
   bench.current++;
   bench.nextRunUs = nowUs() + (uint64_t)bench.pause * SECOND_US;   // after the close, which joins the play's workers
}

static void updateWaiting(uint64_t now)
{
   if (bench.current >= bench.runCount) { finishJob(); return; }
   // someone at the console: X, O or START between runs, or a video they started (not in the job's first seconds -
   // the X that started the app from the XMB may still be down)
   int pressed = isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START);
   if ((pressed && now - bench.jobStartUs >= PAD_GRACE_US) || playScreen.status != SCREEN_TERMINATED) {
      stopJob("stopped from the pad");
      return;
   }
   if (now >= bench.nextRunUs) startRun(now);
}

static void updateRun(uint64_t now)
{
   BenchRunState *on = &bench.on;
   const BenchRun *run = &bench.runs[bench.current];
   // the player closed during a run, which the bench never does: O, from whoever is at the console
   if (playScreen.status == SCREEN_TERMINATED) { stopJob("stopped from the pad"); return; }
   sampleRun();
   if (bench.stage == BENCH_COUNTING) {   // the counting's frames on the UI thread, the one that ends it too (maxgap)
      if (now - on->lastFrameUs > on->frameMaxUs) on->frameMaxUs = now - on->lastFrameUs;
      on->lastFrameUs = now;
   }
   if (on->fallback) { endRun(RESULT_FALLBACK, now); return; }
   if (bench.stage == BENCH_OPENING) {
      if (hasPlayScreenPicture()) {
         on->pictureUs = now;
         bench.stage = BENCH_WARMUP;
         unsigned whole, tenth;
         splitTenths(now - on->startUs, &whole, &tenth);
         logInfo("[bench] run %d/%d: first picture after %u.%u s\n", bench.current + 1, bench.runCount, whole, tenth);
      } else if (hasPlayScreenFailed() || now - on->startUs >= (uint64_t)bench.timeout * SECOND_US) {
         endRun(RESULT_NO_PICTURE, now);
      }
      return;
   }
   if (hasPlayScreenFailed()) { endRun(RESULT_FAILED, now); return; }   // a rebuild that did not open
   VideoPlayer *player = on->player;
   if (player && hasVideoPlayerFailed(player)) {
      // play.c may still go over to H.264 (fallBackFromVp9, by the next frame): "failed" only when it does not
      if (!on->failedSinceUs) on->failedSinceUs = now;
      else if (now - on->failedSinceUs >= FAILED_GRACE_US) { endRun(RESULT_FAILED, now); return; }
   } else {
      on->failedSinceUs = 0;
      if (player && isVideoEnded(player)) { endRun(RESULT_ENDED, now); return; }
   }
   uint64_t lengthUs = (uint64_t)run->seconds * SECOND_US, extraUs = (uint64_t)STUCK_EXTRA_S * SECOND_US;
   if (bench.stage == BENCH_WARMUP) {
      uint64_t warmedUs = now - on->pictureUs;
      if (warmedUs >= (uint64_t)bench.warmup * SECOND_US && player && !on->failedSinceUs) startCounting(now);
      else if (warmedUs >= (uint64_t)bench.warmup * SECOND_US + lengthUs + extraUs) endRun(RESULT_STUCK, now);
      return;
   }
   // `seconds` of the video, by its own clock: a picture that stands still while the clock runs on is not counted as
   // played, and ends as "stuck"
   if (player && on->lastPos - on->basePos >= (float)run->seconds) endRun(RESULT_OK, now);
   else if (now - on->countStartUs >= lengthUs + extraUs) endRun(RESULT_STUCK, now);
}

// ---- bench.h ----

void initBench(void)
{
   const char *folders[2];
   int count = jobFolders(folders);
   // a job that was on when the app last went down, in either place: none runs this start
   int leftover = 0;
   for (int i = 0; i < count; i++)
      if (jobFileExists(folders[i], RUNNING_NAME)) { takeLeftover(folders[i]); leftover = 1; }
   if (leftover) return;
   for (int i = 0; i < count; i++)
      if (jobFileExists(folders[i], JOB_NAME)) { startJob(folders[i]); return; }
}

void updateBench(void)
{
   if (bench.stage == BENCH_OFF) return;
   uint64_t now = nowUs();
   // the frame after a run: the textures its screen retired have gone back by now
   if (bench.memoryDue) { bench.memoryDue = 0; logMemory(bench.memoryRun); }
   if (!bench.jobStartUs) { bench.jobStartUs = now; bench.nextRunUs = now + (uint64_t)bench.startDelay * SECOND_US; }
   // the XMB's Quit (or the console going off) in the middle of a job: not a crash, so no bench.crashed next start
   if (appExitRequested) { stopJob("stopped: the app is closing"); return; }
   if (bench.stage == BENCH_WAITING) updateWaiting(now);
   else updateRun(now);
}

int isBenchActive(void) { return bench.stage != BENCH_OFF; }

int peekBenchGlobal(const char *key, char *value, int cap)
{
   if (!key || !value || cap <= 0) return 0;
   const char *folders[2];
   int count = jobFolders(folders), at = -1;
   for (int i = 0; i < count && at < 0; i++) if (jobFileExists(folders[i], JOB_NAME)) at = i;
   if (at < 0) return 0;
   for (int i = 0; i < count; i++) if (jobFileExists(folders[i], RUNNING_NAME)) return 0;   // a crashed job: none runs
   char path[PATH_CAP];
   jobPath(path, folders[at], JOB_NAME);
   char *text = readWhole(path, JOB_MAX_BYTES, NULL);
   if (!text) return 0;
   int found = 0, number = 0;
   char *cursor = skipBom(text);
   for (char *line; (line = nextLine(&cursor, &number)) != NULL; ) {
      char *name, *setting;
      if (!*line || !splitGlobal(line, &name, &setting) || strcmp(name, key) != 0) continue;
      snprintf(value, (size_t)cap, "%s", setting);   // the last one counts, as for initBench
      found = 1;
   }
   free(text);
   return found;
}

int getBenchPlay(BenchPlay *play)
{
   if (!bench.playOwned) return 0;
   if (play) *play = bench.play;
   return 1;
}

int isBenchPlay(void) { return bench.playOwned; }

void noteBenchVp9Fallback(void)
{
   if (bench.playOwned) bench.on.fallback = 1;
}

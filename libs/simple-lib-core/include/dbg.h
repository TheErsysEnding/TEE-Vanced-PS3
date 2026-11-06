#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "vfs.h"
// local-time source — librtc_stub, PRX-safe. Callers must link -lrtc_stub.
#include <cell/rtc.h>

// vsnprintf from common/printf.c — must NOT use libc_stub's printf family
// from a VSH PRX; see common/printf.h for rationale.
#include "printf.h"

// TEE Vanced: the log goes to a folder of its own in /dev_hdd0/tmp - neither to mohasi's /dev_hdd0/tmp/dbg.txt
// nor to /dev_hdd0/dbg.txt, where v0.11 and v0.12 wrote it. (That line was only ever changed on the build
// machine, so v0.13, built from a clean checkout, fell back to mohasi's path. It lives here now.)
//  - /dev_hdd0/dbg.txt: not every console lets an app create a file directly in /dev_hdd0 (drwxr-xr-x) - the
//    same refusal that sends the data folder from /dev_hdd0/yo-player to a fallback (app-paths.h). On such a
//    console the first v0.13.1 build left no log at all.
//  - /dev_hdd0/tmp/dbg.txt: other ps3-dev homebrew writes that file too, and whichever created it first may have
//    left it writable for itself only (-rw-------). Then nothing can be appended - which is how v0.13 left no
//    log on the same console.
// /dev_hdd0/tmp is open to every app (drwxrwxrwx), and a folder of our own there belongs to nobody else. The
// console clears /dev_hdd0/tmp now and then and openFs never creates a missing parent, so logEmit makes the
// folder when an open fails and tries once more.
#define DBG_DIR "/dev_hdd0/tmp/TEE_Vanced"
#define DBG_LOG DBG_DIR "/dbg.txt"

// Optional live-log callback. If set, every logInfo/Warn/Error call hands
// the fully-formatted line (timestamp + level + body) to the callback
// AFTER the file write. Unset == file only. One process-wide definition
// lives in src/dbg.c so every translation unit shares the same sink — a
// callback installed in one file (e.g. by registerWithBridge) captures
// logs emitted from all of them. simple-debug-bridge wires this to push
// to the host.
typedef void (*LogCallback)(const char *line, int len);
extern LogCallback logCallback;

static inline void setLogCallback(LogCallback fn) { logCallback = fn; }

// Shared bounded-line struct used by every pre-connect log ring in this
// codebase (the producer-side ring in bridge.h and the bridge-side ring
// in server.h). One typedef, one max-line constant — no parallel copies.
#define LOG_LINE_MAX 256

typedef struct {
   int  len;
   char data[LOG_LINE_MAX];
} BacklogLine;

// Format "[HH:MM:SS.mmm] " into out (15 chars, no NUL). Returns the
// byte count, or 0 if the RTC query fails — caller just skips the prefix
// rather than dropping the log line.
static inline int dbgFormatTimestamp(char *out)
{
   CellRtcDateTime d;
   if (cellRtcGetCurrentClockLocalTime(&d) != 0) return 0;

   int hh = (int)d.hour;   if (hh < 0 || hh > 23)   hh = 0;
   int mi = (int)d.minute; if (mi < 0 || mi > 59)   mi = 0;
   int se = (int)d.second; if (se < 0 || se > 59)   se = 0;
   int ms = (int)(d.microsecond / 1000); if (ms < 0 || ms > 999) ms = 0;

   int o = 0;
   out[o++] = '[';
   out[o++] = (char)('0' + (hh / 10) % 10);
   out[o++] = (char)('0' +  hh        % 10);
   out[o++] = ':';
   out[o++] = (char)('0' + (mi / 10) % 10);
   out[o++] = (char)('0' +  mi        % 10);
   out[o++] = ':';
   out[o++] = (char)('0' + (se / 10) % 10);
   out[o++] = (char)('0' +  se        % 10);
   out[o++] = '.';
   out[o++] = (char)('0' + (ms / 100) % 10);
   out[o++] = (char)('0' + (ms /  10) % 10);
   out[o++] = (char)('0' +  ms        % 10);
   out[o++] = ']';
   out[o++] = ' ';
   return o;  // == 15
}

// Append one timestamped, level-prefixed line to DBG_LOG (/dev_hdd0/tmp/TEE_Vanced/dbg.txt in TEE Vanced).
// Variadic: plain string ("hello\n") or printf-style ("rc=0x%x\n", rc).
// Full C99 printf minus floats (see common/printf.h).
//
// Format: "[HH:MM:SS.mmm] [LVL ] <message>"
//         where LVL is INFO / WARN / ERR  (5-char fixed width incl. space).
//
// Thread-safety: builds the timestamp + level + message into one stack
// buffer and issues a SINGLE writeFs. With O_APPEND the kernel positions
// and writes atomically per call, so concurrent loggers from different
// threads/PRX's cannot interleave halves on the same line.
//
// Buffer is 1 KB for the message (any extra is silently dropped) plus 15
// bytes for the timestamp and 7 for the level tag.
static inline void logEmit(const char *level, const char *fmt, va_list ap)
{
   char line[1024 + 32];

   int o = dbgFormatTimestamp(line);  // up to 15 bytes, no NUL

   // "[LVL ] " — 7 bytes, fixed width so log columns align.
   line[o++] = '[';
   line[o++] = level[0];
   line[o++] = level[1];
   line[o++] = level[2];
   line[o++] = level[3];
   line[o++] = ']';
   line[o++] = ' ';

   int n = vsnprintf(line + o, sizeof line - o, fmt, ap);
   if (n <= 0) return;
   int max = (int)sizeof line - o - 1;
   if (n > max) n = max;

   // O_APPEND so the kernel positions+writes atomically per call: concurrent
   // loggers from different threads/PRXs can't interleave halves of a line.
   // The very first line - or the first after the console cleared /dev_hdd0/tmp - finds no DBG_DIR: make it
   // (makeDir: one level, 0 if created or already there) and try once more. The VFS layer never logs, so
   // this cannot recurse.
   VfsFile f;
   int opened = openFs(DBG_LOG, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &f) == 0;
   if (!opened && makeDir(DBG_DIR) == 0)
      opened = openFs(DBG_LOG, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &f) == 0;
   if (opened)
   {
      writeFs(&f, line, (uint64_t)(o + n));
      closeFs(&f);
   }

   if (logCallback) logCallback(line, o + n);
}

static inline void logInfo(const char *fmt, ...)
   __attribute__((format(printf, 1, 2)));
static inline void logWarn(const char *fmt, ...)
   __attribute__((format(printf, 1, 2)));
static inline void logError(const char *fmt, ...)
   __attribute__((format(printf, 1, 2)));

static inline void logInfo(const char *fmt, ...)
{
   va_list ap; va_start(ap, fmt);
   logEmit("INFO", fmt, ap);
   va_end(ap);
}

static inline void logWarn(const char *fmt, ...)
{
   va_list ap; va_start(ap, fmt);
   logEmit("WARN", fmt, ap);
   va_end(ap);
}

static inline void logError(const char *fmt, ...)
{
   va_list ap; va_start(ap, fmt);
   logEmit("ERR ", fmt, ap);
   va_end(ap);
}

// Lines nobody wants by default: one per peer, per packet or per retry, and anything naming what a
// person is doing rather than what the program is doing. They are silent unless the app turns them
// on, so an ordinary log stays small and says nothing about its owner.
extern int logDetailed;

static inline void setLogDetailed(int on) { logDetailed = on; }

static inline void logTrace(const char *fmt, ...)
   __attribute__((format(printf, 1, 2)));

static inline void logTrace(const char *fmt, ...)
{
   if (!logDetailed) return;

   va_list ap; va_start(ap, fmt);
   logEmit("TRCE", fmt, ap);
   va_end(ap);
}

// BUILD_NAME / BUILD_STAMP come from the compiler command line (common/ps3.targets).
// The stamp is "<build number>-<commit>", plus "-dirty" when the build included
// edits that were never committed. Call this first thing at startup so every log
// says which build produced it.
static inline void logBuildVersion(void) { logInfo("%s %s\n", BUILD_NAME, BUILD_STAMP); }

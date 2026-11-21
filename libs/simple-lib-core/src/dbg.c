// dbg - the process-wide log sink pointer declared in dbg.h. lives in a real
// translation unit so every file that logs shares one callback, instead of
// each translation unit getting its own dead copy.
//
// (TEE Vanced V3.1.0 Test 3) also the log's writer thread: see startLogWriter in dbg.h.
#include "dbg.h"
#include "thread.h"
#include <string.h>
#include <sys/sys_time.h>

LogCallback logCallback = 0;
int logDetailed = 0;   // off until an app asks for it, so an ordinary log is short and says little

int (*logQueueLine)(const char *line, int len, int urgent) = 0;

// 128 KB: a busy second of the player logs about 2 KB, a start about 20 KB - the ring is never near full unless the
// writer is starved for seconds, and then the lines are counted, not waited for
#define LOG_RING_BYTES   (128 * 1024)
#define LOG_WRITE_MS     100
#define LOG_REPORT_US    60000000ull   // the longest write of the last minute goes to the log once a minute

static char          logRing[LOG_RING_BYTES];
static int           logRingUsed;
static uint32_t      logLost;           // lines that found the ring full
static char          logOut[LOG_RING_BYTES];   // the writer's copy of the ring (static: no stack holds 128 KB)
static sys_lwmutex_t logRingLock;       // the ring: held for a copy, never for a file write
static sys_lwmutex_t logFileLock;       // the file: the writer thread and an ERR line never write at once
static sys_ppu_thread_t logWriterTid;
static volatile int  logWriterRunning;
static volatile int  logWriterExited;
static int           logWriterStarted;
static uint32_t      logLongestWriteUs;
static uint64_t      logReportUs;

// one append to DBG_LOG, as logEmit does it (the folder made when the console cleared /dev_hdd0/tmp)
static void appendToLog(const char *data, int len)
{
   if (len <= 0) return;
   VfsFile f;
   int opened = openFs(DBG_LOG, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &f) == 0;
   if (!opened && makeDir(DBG_DIR) == 0)
      opened = openFs(DBG_LOG, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &f) == 0;
   if (opened) {
      writeFs(&f, data, (uint64_t)len);
      closeFs(&f);
   }
}

// a line of the log's own, in the usual form ("[HH:MM:SS.mmm] [INFO] ...")
static int formatOwnLine(char *out, int cap, const char *text)
{
   int o = dbgFormatTimestamp(out);
   const char *tag = "[INFO] ";
   for (int i = 0; tag[i] && o < cap - 1; i++) out[o++] = tag[i];
   for (int i = 0; text[i] && o < cap - 1; i++) out[o++] = text[i];
   return o;
}

// with logFileLock held: what the ring holds goes to the file
static void flushRingLocked(void)
{
   lock(&logRingLock);
   int len = logRingUsed;
   if (len) memcpy(logOut, logRing, (size_t)len);
   logRingUsed = 0;
   uint32_t lost = logLost;
   logLost = 0;
   unlock(&logRingLock);
   if (!len && !lost) return;

   uint64_t startUs = sys_time_get_system_time();
   appendToLog(logOut, len);
   if (lost) {
      char text[96], line[128];
      snprintf(text, sizeof text, "[log] %u lines lost: the ring was full (the writer had no time)\n", (unsigned)lost);
      appendToLog(line, formatOwnLine(line, sizeof line, text));
   }
   uint32_t tookUs = (uint32_t)(sys_time_get_system_time() - startUs);
   if (tookUs > logLongestWriteUs) logLongestWriteUs = tookUs;
}

static int queueLogLine(const char *line, int len, int urgent)
{
   if (urgent) {   // an error: everything before it, then the line itself, now - on the thread that logged it
      lock(&logFileLock);
      flushRingLocked();
      appendToLog(line, len);
      unlock(&logFileLock);
      return 1;
   }
   lock(&logRingLock);
   if (logRingUsed + len <= LOG_RING_BYTES) {
      memcpy(logRing + logRingUsed, line, (size_t)len);
      logRingUsed += len;
   } else {
      logLost++;
   }
   unlock(&logRingLock);
   return 1;
}

static void logWriterThread(uint64_t arg)
{
   (void)arg;
   while (logWriterRunning) {
      sleepMs(LOG_WRITE_MS);
      lock(&logFileLock);
      flushRingLocked();
      unlock(&logFileLock);
      // once a minute: what the threads that log no longer pay for a write
      uint64_t nowUs = sys_time_get_system_time();
      if (!logReportUs) logReportUs = nowUs;
      else if (nowUs - logReportUs >= LOG_REPORT_US) {
         logReportUs = nowUs;
         char text[96];
         snprintf(text, sizeof text, "[log] longest write in the last minute: %u us\n", (unsigned)logLongestWriteUs);
         logLongestWriteUs = 0;
         char line[128];
         queueLogLine(line, formatOwnLine(line, sizeof line, text), 0);
      }
   }
   __sync_synchronize();
   logWriterExited = 1;
   exitThread();
}

void startLogWriter(void)
{
   if (logWriterStarted) return;
   if (createLock(&logRingLock) != 0) return;
   if (createLock(&logFileLock) != 0) { destroyLock(&logRingLock); return; }
   logWriterRunning = 1;
   logWriterExited = 0;
   if (spawnJoinableThread(&logWriterTid, logWriterThread, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_16KB, "tv_log") != 0) {
      logWriterRunning = 0;
      destroyLock(&logFileLock);
      destroyLock(&logRingLock);
      logWarn("[log] no writer thread: every line is written at once\n");
      return;
   }
   logWriterStarted = 1;
   __sync_synchronize();
   logQueueLine = queueLogLine;
   logInfo("[log] written by a thread of its own every %d ms (errors at once)\n", LOG_WRITE_MS);
}

void stopLogWriter(void)
{
   if (!logWriterStarted) return;
   logQueueLine = 0;          // from here on every line goes straight to the file again
   __sync_synchronize();
   logWriterRunning = 0;
   for (int waited = 0; !logWriterExited && waited < 2000; waited += 5) sleepMs(5);
   uint64_t exitCode;
   if (logWriterExited) sys_ppu_thread_join(logWriterTid, &exitCode);
   // what was queued meanwhile (a line in flight while logQueueLine went), unless the writer hangs in a write
   if (logWriterExited) {
      lock(&logFileLock);
      flushRingLocked();
      unlock(&logFileLock);
   }
   logWriterStarted = 0;
}

// update-check - implementation (see update-check.h).

#include "update-check.h"
#include "app-paths.h"          // APP_VERSION
#include "settings.h"           // getUpdateCheck
#include "http.h"               // fetchHttp
#include "thread.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>              // snprintf

#define CHANGELOG_URL  "https://raw.githubusercontent.com/TheErsysEnding/TEE-Vanced-PS3/main/CHANGELOG.md"
#define CHANGELOG_HEAD 16384   // the top of it: the newest release's heading is in its first lines

static sys_ppu_thread_t worker;
static int              workerActive;
static volatile int     newerFound;
static char             newerVersion[16];

// "3.0.0" in text (after a 'v' or 'V') -> its three numbers; 0 when it is none
static int readVersion(const char *text, int out[3])
{
   const char *at = text;
   if (*at == 'v' || *at == 'V') at++;
   for (int part = 0; part < 3; part++) {
      if (*at < '0' || *at > '9') return 0;
      out[part] = 0;
      while (*at >= '0' && *at <= '9') out[part] = out[part] * 10 + (*at++ - '0');
      if (part < 2) { if (*at != '.') return 0; at++; }
   }
   return 1;
}

static int isNewer(const int a[3], const int b[3])   // a > b
{
   for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
   return 0;
}

static void checkThread(uint64_t arg)
{
   (void)arg;
   char *text = (char *)malloc(CHANGELOG_HEAD + 1);
   int own[3] = { 0, 0, 0 };
   if (text && readVersion(APP_VERSION, own)) {
      HttpHeader headers[] = { { "User-Agent", "TEE-Vanced-PS3" }, { "Range", "bytes=0-16383" } };
      int length = 0, status = 0;
      if (fetchHttp("GET", CHANGELOG_URL, headers, 2, NULL, 0, text, CHANGELOG_HEAD + 1, &length, &status) == 0 &&
          (status == 200 || status == 206) && length > 0) {
         text[length < CHANGELOG_HEAD ? length : CHANGELOG_HEAD] = 0;
         // the first heading that is a release: "## v2.0.0 — 2026-11-07" (not "## v3.0.0 — not released yet")
         for (char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            int release[3];
            if (strncmp(line, "## ", 3) != 0 || !readVersion(line + 3, release)) continue;
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            int unreleased = strstr(line, "not released") != NULL;
            if (end) *end = '\n';
            if (unreleased) continue;
            if (isNewer(release, own)) {
               snprintf(newerVersion, sizeof newerVersion, "%d.%d.%d", release[0], release[1], release[2]);
               __sync_synchronize();
               newerFound = 1;
               logInfo("[update] TEE Vanced v%s is out (this is %s)\n", newerVersion, APP_VERSION);
            } else {
               logInfo("[update] the newest release is v%d.%d.%d: nothing newer than %s\n", release[0], release[1],
                       release[2], APP_VERSION);
            }
            break;
         }
      } else {
         logWarn("[update] the release list did not come (HTTP %d)\n", status);
      }
   }
   free(text);
   exitThread();
}

void startUpdateCheck(void)
{
   if (workerActive) return;
   if (!getUpdateCheck()) { logInfo("[update] off (settings.txt update-check=off)\n"); return; }
   workerActive = spawnJoinableThread(&worker, checkThread, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                                      "tee-update") == 0;
}

void stopUpdateCheck(void)
{
   if (workerActive) { joinThread(worker); workerActive = 0; }
}

int getNewerRelease(char *out, int cap)
{
   if (!newerFound) return 0;
   if (out && cap > 0) strCopy(out, cap, newerVersion);
   return 1;
}

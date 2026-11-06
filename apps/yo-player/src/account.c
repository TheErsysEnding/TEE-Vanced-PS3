// account - implementation (see account.h).

#include "account.h"
#include "extractor.h"          // getAccountFeed / changeAccount: the account's lists and changes (youtube.c)
#include "app-paths.h"          // getAppDataDir: account.txt lives with the other data
#include "http.h"               // fetchHttp
#include "vfs.h"                // readFile / writeFile / deleteFile
#include "thread.h"             // lock/unlock, spawnThread for the sign-out notice
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <sys/sys_time.h>       // sys_time_get_system_time: when the access token runs out
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// YouTube's TV app as it introduces itself to Google: its public client id and secret (both are in the TV app's own
// JavaScript), and the scopes it asks for - full YouTube access, nothing outside YouTube.
#define CLIENT_ID     "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com"
#define CLIENT_SECRET "SboVhoG9s0rNafixCSGGKXAT"
#define SCOPE         "http://gdata.youtube.com https://www.googleapis.com/auth/youtube-paid-content"
#define CODE_URL      "https://www.youtube.com/o/oauth2/device/code"
#define TOKEN_URL     "https://www.youtube.com/o/oauth2/token"
#define REVOKE_URL    "https://oauth2.googleapis.com/revoke"
#define DEVICE_GRANT  "http://oauth.net/grant_type/device/1.0"
#define DEFAULT_ADDRESS "https://www.google.com/device"

#define REPLY_CAP         16384   // a token reply is under 3 KB
#define REFRESH_CAP       512
#define ACCESS_CAP        2560
#define RENEW_EARLY_US    (60ull * 1000000ull)    // renew a minute before Google would refuse the old one
#define RENEW_RETRY_US    (15ull * 1000000ull)    // after a failed renewal, the lists fail fast this long
#define FILE_CAP          2048

static sys_lwmutex_t accountLock;
static int      lockReady;
static char     refreshToken[REFRESH_CAP];   // the sign-in itself; "" = signed out
static char     accessToken[ACCESS_CAP];     // short-lived; memory only, renewed from refreshToken
static uint64_t accessExpiresUs;             // sys_time_get_system_time() at which accessToken stops working
static uint64_t renewFailedUs;               // when the last renewal failed, 0 = it did not
static volatile unsigned revision;

static void accountPath(char *out, int cap) { snprintf(out, cap, "%s/account.txt", getAppDataDir()); }

// A token or device code as it may go into a header line and a JSON string: nothing that could end either early.
static int isToken(const char *text)
{
   if (!text[0]) return 0;
   for (const char *p = text; *p; p++) {
      char c = *p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
      if (c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=') continue;
      return 0;
   }
   return 1;
}

// the code the viewer types, as Google writes it: capitals, digits and dashes
static int isUserCode(const char *text)
{
   if (!text[0]) return 0;
   for (const char *p = text; *p; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
   return 1;
}

// The value of "key" in one of Google's small OAuth replies, as text: a string's characters (tokens and codes have
// no escapes) or a number's digits. 0 when the key is absent or its value did not fit - a cut token is no token.
static int replyValue(const char *reply, const char *key, char *out, int cap)
{
   char pattern[48];
   snprintf(pattern, sizeof pattern, "\"%s\"", key);
   out[0] = 0;
   const char *p = strstr(reply, pattern);
   if (!p) return 0;
   p += strlen(pattern);
   while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
   int i = 0;
   if (*p == '"') {
      for (p++; *p && *p != '"' && i < cap - 1; ) out[i++] = *p++;
      out[i] = 0;
      return *p == '"';
   }
   while (((*p >= '0' && *p <= '9') || *p == '-') && i < cap - 1) out[i++] = *p++;
   out[i] = 0;
   return i > 0;
}

static int replyNumber(const char *reply, const char *key, int fallback)
{
   char digits[16];
   return replyValue(reply, key, digits, sizeof digits) ? atoi(digits) : fallback;
}

static int postJson(const char *url, const char *body, char *reply, int cap, int *status)
{
   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", ACCOUNT_TV_USER_AGENT },
   };
   int length = 0;
   *status = 0;
   int rc = fetchHttp("POST", url, headers, 2, body, (int)strlen(body), reply, cap, &length, status);
   if (rc < 0) reply[0] = 0;
   return rc;
}

// A fresh device id for each sign-in, as the TV app sends one: 32 hex digits. It only has to differ between
// consoles and sign-ins, not be secret.
static void makeDeviceId(char *out, int cap)
{
   uint64_t seed = sys_time_get_system_time() ^ ((uint64_t)(uintptr_t)out << 21) ^ 0x9E3779B97F4A7C15ull;
   int i = 0;
   for (; i < 32 && i < cap - 1; i++) {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      out[i] = "0123456789abcdef"[seed & 15];
   }
   out[i] = 0;
}

// ---- the stored sign-in (callers hold accountLock) ----

static int saveSignIn(void)
{
   char path[160], text[FILE_CAP];
   accountPath(path, sizeof path);
   int length = snprintf(text, sizeof text,
      "# TEE Vanced - your YouTube sign-in. Whoever has this file can use your YouTube account (not your\n"
      "# Google password). Sign out in Settings to withdraw it at Google; keep the console's FTP to yourself.\n"
      "refresh=%s\n", refreshToken);
   if (length <= 0 || length >= (int)sizeof text) return -1;
   return writeFile(path, text, (uint64_t)length) < 0 ? -1 : 0;
}

static void forgetSignIn(void)
{
   char path[160];
   accountPath(path, sizeof path);
   memset(refreshToken, 0, sizeof refreshToken);
   memset(accessToken, 0, sizeof accessToken);
   accessExpiresUs = renewFailedUs = 0;
   deleteFile(path);
   revision++;
}

void initAccount(void)
{
   if (!lockReady && createLock(&accountLock) == 0) lockReady = 1;
   char path[160];
   accountPath(path, sizeof path);
   char *text = (char *)malloc(FILE_CAP);
   if (!text) return;
   int length = readFile(path, text, FILE_CAP - 1);
   text[length > 0 ? length : 0] = 0;

   const char *line = strstr(text, "\nrefresh=");
   if (line) line += 1;
   else if (strncmp(text, "refresh=", 8) == 0) line = text;
   if (line) {
      line += 8;
      int i = 0;
      while (line[i] && line[i] != '\r' && line[i] != '\n' && i < REFRESH_CAP - 1) { refreshToken[i] = line[i]; i++; }
      refreshToken[i] = 0;
      if (!isToken(refreshToken)) memset(refreshToken, 0, sizeof refreshToken);
   }
   memset(text, 0, FILE_CAP);
   free(text);
   if (length > 0 && !refreshToken[0]) logWarn("[account] account.txt holds no usable sign-in: signed out\n");
   logInfo("[account] %s\n", refreshToken[0] ? "signed in (kept from an earlier start)" : "not signed in");
}

int isAccountSignedIn(void) { return refreshToken[0] != 0; }

unsigned getAccountRevision(void) { return revision; }

// ---- signing in ----

int requestAccountCode(AccountCode *out)
{
   memset(out, 0, sizeof *out);
   char deviceId[40], body[512];
   makeDeviceId(deviceId, sizeof deviceId);
   snprintf(body, sizeof body, "{\"client_id\":\"%s\",\"scope\":\"%s\",\"device_id\":\"%s\",\"device_model\":\"ytlr::\"}",
            CLIENT_ID, SCOPE, deviceId);
   char *reply = (char *)malloc(REPLY_CAP);
   if (!reply) return -1;
   int status = 0;
   int rc = postJson(CODE_URL, body, reply, REPLY_CAP, &status);
   int ok = rc == 0 && status == 200 &&
            replyValue(reply, "user_code", out->userCode, sizeof out->userCode) && isUserCode(out->userCode) &&
            replyValue(reply, "device_code", out->deviceCode, sizeof out->deviceCode) && isToken(out->deviceCode);
   if (ok) {
      if (!replyValue(reply, "verification_url", out->address, sizeof out->address))
         strCopy(out->address, sizeof out->address, DEFAULT_ADDRESS);
      out->intervalSeconds  = replyNumber(reply, "interval", 5);
      out->expiresInSeconds = replyNumber(reply, "expires_in", 1800);
      if (out->intervalSeconds < 1 || out->intervalSeconds > 60) out->intervalSeconds = 5;
      if (out->expiresInSeconds < 60 || out->expiresInSeconds > 7200) out->expiresInSeconds = 1800;
      logInfo("[account] sign-in code received: good for %d min, asked every %d s\n", out->expiresInSeconds / 60,
              out->intervalSeconds);
   } else {
      char error[48];
      replyValue(reply, "error", error, sizeof error);
      logError("[account] no sign-in code: rc=%d HTTP %d %s\n", rc, status, error);
      memset(out, 0, sizeof *out);
   }
   free(reply);
   return ok ? 0 : -1;
}

AccountPoll pollAccountCode(const AccountCode *code)
{
   if (!isToken(code->deviceCode)) return ACCOUNT_POLL_FAILED;
   char body[768];
   snprintf(body, sizeof body,
            "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"code\":\"%s\",\"grant_type\":\"" DEVICE_GRANT "\"}",
            CLIENT_ID, CLIENT_SECRET, code->deviceCode);
   char *reply = (char *)malloc(REPLY_CAP);
   char *refresh = (char *)malloc(REFRESH_CAP), *access = (char *)malloc(ACCESS_CAP);
   if (!reply || !refresh || !access) { free(reply); free(refresh); free(access); return ACCOUNT_POLL_FAILED; }

   AccountPoll result = ACCOUNT_POLL_FAILED;
   int status = 0;
   int rc = postJson(TOKEN_URL, body, reply, REPLY_CAP, &status);
   char error[48];
   if (rc < 0) {
      logWarn("[account] sign-in check: no answer (rc=%d)\n", rc);
   } else if (replyValue(reply, "error", error, sizeof error)) {
      if      (strcmp(error, "authorization_pending") == 0) result = ACCOUNT_POLL_PENDING;
      else if (strcmp(error, "slow_down") == 0)             result = ACCOUNT_POLL_SLOW_DOWN;
      else if (strcmp(error, "access_denied") == 0)         result = ACCOUNT_POLL_DENIED;
      else if (strcmp(error, "expired_token") == 0)         result = ACCOUNT_POLL_EXPIRED;
      if (result != ACCOUNT_POLL_PENDING) logInfo("[account] sign-in check: HTTP %d %s\n", status, error);
   } else if (status == 200 &&
              replyValue(reply, "refresh_token", refresh, REFRESH_CAP) && isToken(refresh) &&
              replyValue(reply, "access_token", access, ACCESS_CAP) && isToken(access)) {
      int seconds = replyNumber(reply, "expires_in", 3600);
      lock(&accountLock);
      strCopy(refreshToken, sizeof refreshToken, refresh);
      strCopy(accessToken, sizeof accessToken, access);
      accessExpiresUs = sys_time_get_system_time() + (uint64_t)seconds * 1000000ull;
      renewFailedUs = 0;
      int saved = saveSignIn() == 0;
      revision++;
      unlock(&accountLock);
      logInfo("[account] signed in: access good for %d min, sign-in %s\n", seconds / 60,
              saved ? "saved" : "NOT saved (the next start asks again)");
      result = ACCOUNT_POLL_GRANTED;
   } else {
      logWarn("[account] sign-in check: HTTP %d without a token\n", status);
   }
   memset(reply, 0, REPLY_CAP);
   memset(refresh, 0, REFRESH_CAP);
   memset(access, 0, ACCESS_CAP);
   free(reply); free(refresh); free(access);
   return result;
}

// ---- using it ----

// A new access token from the refresh token. Called with accountLock held, so every list that needs one at the
// same moment waits for this single request instead of each sending its own.
static void renewAccessToken(void)
{
   char *body = (char *)malloc(1024), *reply = (char *)malloc(REPLY_CAP);
   if (!body || !reply) { free(body); free(reply); return; }
   snprintf(body, 1024,
            "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"refresh_token\":\"%s\",\"grant_type\":\"refresh_token\"}",
            CLIENT_ID, CLIENT_SECRET, refreshToken);
   int status = 0;
   int rc = postJson(TOKEN_URL, body, reply, REPLY_CAP, &status);
   if (rc == 0 && status == 200 && replyValue(reply, "access_token", accessToken, sizeof accessToken) &&
       isToken(accessToken)) {
      int seconds = replyNumber(reply, "expires_in", 3600);
      accessExpiresUs = sys_time_get_system_time() + (uint64_t)seconds * 1000000ull;
      renewFailedUs = 0;
      logInfo("[account] access renewed: good for %d min\n", seconds / 60);
   } else {
      char error[48];
      replyValue(reply, "error", error, sizeof error);
      memset(accessToken, 0, sizeof accessToken);
      if (strcmp(error, "invalid_grant") == 0) {
         forgetSignIn();
         logWarn("[account] Google no longer accepts this sign-in (withdrawn in the Google account?): signed out\n");
      } else {
         renewFailedUs = sys_time_get_system_time();
         logError("[account] access could not be renewed: rc=%d HTTP %d %s\n", rc, status, error);
      }
   }
   memset(body, 0, 1024);
   memset(reply, 0, REPLY_CAP);
   free(body); free(reply);
}

int getAccountAuthorization(char *out, int cap)
{
   out[0] = 0;
   if (!lockReady || !refreshToken[0]) return -1;
   int rc = -1;
   lock(&accountLock);
   if (refreshToken[0]) {
      uint64_t now = sys_time_get_system_time();
      int due = !accessToken[0] || now + RENEW_EARLY_US >= accessExpiresUs;
      int resting = renewFailedUs && now - renewFailedUs < RENEW_RETRY_US;
      if (due && !resting) renewAccessToken();
      if (accessToken[0] && (int)strlen(accessToken) + 8 < cap) {
         snprintf(out, cap, "Bearer %s", accessToken);
         rc = 0;
      }
   }
   unlock(&accountLock);
   return rc;
}

void dropAccountAccess(void)
{
   if (!lockReady) return;
   lock(&accountLock);
   memset(accessToken, 0, sizeof accessToken);
   unlock(&accountLock);
}

// ---- signing out ----

// Google is told the sign-in is withdrawn, so it also leaves the list in the Google account. Runs on its own
// detached thread with its own copy of the token: the sign-out on screen has already happened.
static void revokeWorker(uint64_t arg)
{
   char *token = (char *)(uintptr_t)arg;
   char body[3 * REFRESH_CAP + 8];
   int length = snprintf(body, sizeof body, "token=");
   for (const char *p = token; *p && length < (int)sizeof body - 4; p++) {   // form-encoded: '/' and '+' escaped
      char c = *p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
          c == '_' || c == '~') body[length++] = c;
      else length += snprintf(body + length, sizeof body - length, "%%%02X", (unsigned char)c);
   }
   body[length] = 0;
   HttpHeader headers[] = {
      { "Content-Type", "application/x-www-form-urlencoded" },
      { "User-Agent", ACCOUNT_TV_USER_AGENT },
   };
   char reply[512];
   int replyLength = 0, status = 0;
   int rc = fetchHttp("POST", REVOKE_URL, headers, 2, body, length, reply, sizeof reply, &replyLength, &status);
   if (rc == 0 && status == 200) logInfo("[account] Google has withdrawn the sign-in as well\n");
   else logWarn("[account] Google was not told (rc=%d HTTP %d): remove the sign-in in the Google account\n", rc, status);
   memset(body, 0, sizeof body);
   memset(token, 0, strlen(token));
   free(token);
   exitThread();
}

void signOutAccount(void)
{
   if (!lockReady) return;
   lock(&accountLock);
   char *copy = NULL;
   if (refreshToken[0]) {
      copy = (char *)malloc(REFRESH_CAP);
      if (copy) strCopy(copy, REFRESH_CAP, refreshToken);
   }
   forgetSignIn();
   unlock(&accountLock);
   logInfo("[account] signed out\n");
   if (!copy) return;
   sys_ppu_thread_t tid;
   if (spawnThread(&tid, revokeWorker, (uint64_t)(uintptr_t)copy, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                   "account-revoke") != 0) {
      logWarn("[account] Google could not be told: remove the sign-in in the Google account\n");
      memset(copy, 0, REFRESH_CAP);
      free(copy);
   }
}

// ---- the account's own lists (account.h) ----

#define MAX_FOLLOWED   4000                        // YouTube lets an account follow 2000 channels
#define MAX_LATER      1000                        // Watch Later as far as its first pages go
#define LIST_PAGES     12                          // pages read per list at the most
#define LISTS_RETRY_US (60ull * 1000000ull)        // a failed load is tried again this much later
#define MAX_PLAYLISTS_KEPT 100

typedef char ListId[32];

static sys_lwmutex_t listsLock, changeLock;        // the lists; one change on the network at a time, in order
static int           listsReady;
static ListId       *followed, *later;             // channel ids, video ids
static int           followedCount, laterCount;
static AccountPlaylistInfo *playlists;
static int           playlistCount;
static volatile int  reloadWanted;                 // refreshAccountLists: read everything again
static unsigned      listsLoadedFor = 0xFFFFFFFFu;  // the sign-in revision the lists belong to
static volatile int  listsLoading;
static uint64_t      listsFailedUs;
static volatile unsigned listsRevision, changesRevision;
static char          failureText[160];
static int           failurePending;

static int indexOf(ListId *ids, int count, const char *id)   // not const: C will not pass a ListId * as one
{
   for (int i = 0; i < count; i++) if (strcmp(ids[i], id) == 0) return i;
   return -1;
}

static void addId(ListId *ids, int *count, int cap, const char *id)
{
   if (indexOf(ids, *count, id) >= 0 || *count >= cap) return;
   strCopy(ids[*count], sizeof ids[0], id);
   (*count)++;
}

static void removeId(ListId *ids, int *count, const char *id)
{
   int i = indexOf(ids, *count, id);
   if (i < 0) return;
   memmove(ids + i, ids + i + 1, (size_t)(*count - i - 1) * sizeof ids[0]);
   (*count)--;
}

// UI thread only, so the first call needs no lock of its own
static int ensureLists(void)
{
   if (!listsReady) {
      if (createLock(&listsLock) != 0 || createLock(&changeLock) != 0) return 0;
      listsReady = 1;
   }
   if (!followed) followed = (ListId *)calloc(MAX_FOLLOWED, sizeof *followed);
   if (!later)    later    = (ListId *)calloc(MAX_LATER, sizeof *later);
   if (!playlists) playlists = (AccountPlaylistInfo *)calloc(MAX_PLAYLISTS_KEPT, sizeof *playlists);
   return followed && later && playlists;
}

// the lists hold what belongs to the sign-in in force (the caller holds listsLock)
static int listsCurrent(void) { return refreshToken[0] && listsLoadedFor == revision; }

static void listsWorker(uint64_t arg)
{
   unsigned forRevision = (unsigned)arg;
   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   char *token = (char *)malloc(MAX_CONTINUATION);
   ListId *newFollowed = (ListId *)calloc(MAX_FOLLOWED, sizeof *newFollowed);
   ListId *newLater    = (ListId *)calloc(MAX_LATER, sizeof *newLater);
   AccountPlaylistInfo *newPlaylists = (AccountPlaylistInfo *)calloc(MAX_PLAYLISTS_KEPT, sizeof *newPlaylists);
   int ok = page && token && newFollowed && newLater && newPlaylists, followedFound = 0, laterFound = 0;
   int playlistsFound = 0;
   for (int list = 0; ok && list < 2; list++) {
      AccountFeed feed = list == 0 ? ACCOUNT_FEED_CHANNELS : ACCOUNT_FEED_WATCH_LATER;
      token[0] = 0;
      for (int pageNumber = 0; pageNumber < LIST_PAGES; pageNumber++) {
         if (getAccountFeed(feed, token[0] ? token : NULL, page) != 0) { ok = 0; break; }
         for (int i = 0; i < page->count; i++) {
            if (list == 0) {
               if (page->items[i].channelId[0]) addId(newFollowed, &followedFound, MAX_FOLLOWED, page->items[i].channelId);
            }
            else addId(newLater, &laterFound, MAX_LATER, page->items[i].videoId);
         }
         if (!page->continuation[0]) break;
         strCopy(token, MAX_CONTINUATION, page->continuation);
      }
   }
   if (ok) {   // the playlists are a page of their own: one that fails leaves the other two lists in force
      playlistsFound = getAccountPlaylists(newPlaylists, MAX_PLAYLISTS_KEPT);
      if (playlistsFound < 0) playlistsFound = 0;
   }
   lock(&listsLock);
   if (ok && forRevision == revision) {
      memcpy(playlists, newPlaylists, (size_t)playlistsFound * sizeof *playlists);
      playlistCount = playlistsFound;
      reloadWanted = 0;
      memcpy(followed, newFollowed, (size_t)followedFound * sizeof *followed);
      followedCount = followedFound;
      memcpy(later, newLater, (size_t)laterFound * sizeof *later);
      laterCount = laterFound;
      listsLoadedFor = forRevision;
      listsRevision++;
   }
   if (!ok) listsFailedUs = sys_time_get_system_time();
   listsLoading = 0;
   unlock(&listsLock);
   if (ok) logInfo("[account] lists loaded: %d channels followed, %d videos in Watch Later, %d playlists\n",
                   followedFound, laterFound, playlistsFound);
   else    logWarn("[account] the account's lists could not be loaded: asked for again in a minute\n");
   free(page); free(token); free(newFollowed); free(newLater); free(newPlaylists);
   exitThread();
}

void syncAccountLists(void)
{
   if (!refreshToken[0] || listsLoading || !ensureLists()) return;
   unsigned current = revision;
   if (listsLoadedFor == current && !reloadWanted) return;
   uint64_t now = sys_time_get_system_time();
   if (listsFailedUs && now - listsFailedUs < LISTS_RETRY_US) return;
   listsLoading = 1;
   sys_ppu_thread_t tid;
   if (spawnThread(&tid, listsWorker, (uint64_t)current, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                   "account-lists") != 0) {
      listsLoading = 0;
      listsFailedUs = now;
   }
}

int isAccountFollowing(const char *channelId)
{
   if (!listsReady || !channelId[0]) return 0;
   lock(&listsLock);
   int found = listsCurrent() && indexOf(followed, followedCount, channelId) >= 0;
   unlock(&listsLock);
   return found;
}

int isInAccountWatchLater(const char *videoId)
{
   if (!listsReady || !videoId[0]) return 0;
   lock(&listsLock);
   int found = listsCurrent() && indexOf(later, laterCount, videoId) >= 0;
   unlock(&listsLock);
   return found;
}

unsigned getAccountListsRevision(void)   { return listsRevision; }

int getAccountFollowingCount(void)
{
   if (!listsReady) return 0;
   lock(&listsLock);
   int count = listsCurrent() ? followedCount : 0;
   unlock(&listsLock);
   return count;
}

int getAccountWatchLaterCount(void)
{
   if (!listsReady) return -1;
   lock(&listsLock);
   int count = listsCurrent() ? laterCount : -1;
   unlock(&listsLock);
   return count;
}

int getAccountPlaylistCount(void)
{
   if (!listsReady) return 0;
   lock(&listsLock);
   int count = listsCurrent() ? playlistCount : 0;
   unlock(&listsLock);
   return count;
}

int getAccountPlaylistAt(int index, AccountPlaylistInfo *out)
{
   if (!listsReady) return 0;
   lock(&listsLock);
   int found = listsCurrent() && index >= 0 && index < playlistCount;
   if (found) *out = playlists[index];
   unlock(&listsLock);
   return found;
}

void refreshAccountLists(void)
{
   reloadWanted = 1;
   listsFailedUs = 0;   // asked for now: not held back by an earlier failure
}
unsigned getAccountChangesRevision(void) { return changesRevision; }

typedef struct { AccountAction action; char id[32]; } AccountChange;

// a change in memory, applied or undone (listsLock held)
static void applyChange(AccountAction action, const char *id, int undo)
{
   int add = (action == ACCOUNT_ADD_WATCH_LATER || action == ACCOUNT_SUBSCRIBE) != undo;
   int toLater = action == ACCOUNT_ADD_WATCH_LATER || action == ACCOUNT_REMOVE_WATCH_LATER;
   if (toLater) { if (add) addId(later, &laterCount, MAX_LATER, id); else removeId(later, &laterCount, id); }
   else         { if (add) addId(followed, &followedCount, MAX_FOLLOWED, id); else removeId(followed, &followedCount, id); }
   listsRevision++;
   changesRevision++;
}

static void refuseChange(const AccountChange *change)
{
   static const char *WHAT[] = {
      "YouTube did not add the video to Watch Later.",
      "YouTube did not remove the video from Watch Later.",
      "YouTube did not take the subscription.",
      "YouTube did not take the unsubscribe.",
   };
   lock(&listsLock);
   applyChange(change->action, change->id, 1);
   snprintf(failureText, sizeof failureText, "%s Check the connection and try again.", WHAT[change->action]);
   failurePending = 1;
   unlock(&listsLock);
}

static void changeWorker(uint64_t arg)
{
   AccountChange *change = (AccountChange *)(uintptr_t)arg;
   lock(&changeLock);
   int rc = changeAccount(change->action, change->id);
   unlock(&changeLock);
   if (rc != 0) refuseChange(change);
   free(change);
   exitThread();
}

// in memory at once, then to YouTube on a worker; undone if YouTube refuses it
static void makeChange(AccountAction action, const char *id)
{
   if (!refreshToken[0] || !id[0] || !ensureLists()) return;
   AccountChange *change = (AccountChange *)malloc(sizeof *change);
   if (!change) return;
   change->action = action;
   strCopy(change->id, sizeof change->id, id);
   lock(&listsLock);
   applyChange(action, id, 0);
   unlock(&listsLock);
   sys_ppu_thread_t tid;
   if (spawnThread(&tid, changeWorker, (uint64_t)(uintptr_t)change, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB,
                   "account-change") != 0) {
      refuseChange(change);
      free(change);
   }
}

void setAccountFollowing(const char *channelId, int follow)
{
   makeChange(follow ? ACCOUNT_SUBSCRIBE : ACCOUNT_UNSUBSCRIBE, channelId);
}

void setAccountWatchLater(const char *videoId, int add)
{
   makeChange(add ? ACCOUNT_ADD_WATCH_LATER : ACCOUNT_REMOVE_WATCH_LATER, videoId);
}

int takeAccountFailure(char *out, int cap)
{
   if (!listsReady) return 0;
   lock(&listsLock);
   int had = failurePending;
   if (had) { strCopy(out, cap, failureText); failurePending = 0; }
   unlock(&listsLock);
   return had;
}

// storage - plain-text preferences + watch history under the app data directory (see storage.h).

#include "storage.h"
#include "app-paths.h"
#include "vfs.h"
#include "string-utilities.h"   // strCopy
#include "settings.h"           // sponsorblock-mode: the first-run default for the skip mask
#include "sponsorblock.h"       // SponsorCategory, for the seeded default
#include "dbg.h"                // logInfo / logError
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Resolved at startup (chooseDataDir): the app tries the persistent location first and falls back to one it
// can prove is writable, rather than trusting a single hard-coded guess - which is what left the user with a
// subscribe button that could never save anything.
static char dataDir[64];
static char historyPath[96], subsPath[96], watchLaterPath[96], prefsPath[96], searchPath[96];
static char watchedPath[96], migrateMarker[96], playlistsPath[96];

const char *getAppDataDir(void) { return dataDir[0] ? dataDir : YO_DATA_DIR; }

static void useDataDir(const char *dir)
{
   strCopy(dataDir, sizeof dataDir, dir);
   snprintf(historyPath,    sizeof historyPath,    "%s/history.txt",       dataDir);
   snprintf(subsPath,       sizeof subsPath,       "%s/subscriptions.txt", dataDir);
   snprintf(watchLaterPath, sizeof watchLaterPath, "%s/watchlater.txt",    dataDir);
   snprintf(prefsPath,      sizeof prefsPath,      "%s/prefs.txt",         dataDir);
   snprintf(searchPath,     sizeof searchPath,     "%s/searches.txt",      dataDir);
   snprintf(watchedPath,    sizeof watchedPath,    "%s/watched.txt",       dataDir);
   snprintf(migrateMarker,  sizeof migrateMarker,  "%s/.migrated",         dataDir);
   snprintf(playlistsPath,  sizeof playlistsPath,  "%s/playlists.txt",     dataDir);
}

#define SUBS_LINE_MAX  (CHANNEL_ID_LEN + CHANNEL_NAME_LEN + 2)   // "id<TAB>name\n"
#define SUBS_BUFFER_BYTES (MAX_SUBSCRIPTIONS * SUBS_LINE_MAX + 16)

#define SEARCH_HISTORY_BUFFER_BYTES (MAX_SEARCH_HISTORY * (SEARCH_QUERY_LEN + 2) + 16)

#define MAX_WATCHLATER          200
#define WATCHLATER_LINE_MAX     480     // one serialized SearchResult (8 tab-separated fields + newline)
#define WATCHLATER_BUFFER_BYTES (MAX_WATCHLATER * WATCHLATER_LINE_MAX + 16)

#define HISTORY_CAP    1000     // watched videos kept (oldest dropped past this)
#define VIDEO_ID_LEN   12       // 11 chars + NUL
#define ENTRY_LINE_MAX 24       // "videoId seconds\n"
#define HISTORY_BUFFER_BYTES (HISTORY_CAP * ENTRY_LINE_MAX + 16)   // whole-file load/save buffer

// watch history held in memory; the file is the durable copy. a plain ring: once full, the oldest slot is
// overwritten (rare - 1000 videos), and the file is rewritten from the live set. each entry also carries the
// last playback position for resume.
typedef struct { char id[VIDEO_ID_LEN]; int position; } WatchEntry;
static WatchEntry watched[HISTORY_CAP];
static int        watchedCount;
static int        watchedNext;  // next slot to write when the ring is full

static void loadHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(historyPath, buffer, HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && watchedCount < HISTORY_CAP; line = strtok(NULL, "\r\n")) {
         char id[VIDEO_ID_LEN]; int position = 0;
         if (line[0] && sscanf(line, "%11s %d", id, &position) >= 1) {   // second field optional (old files)
            strCopy(watched[watchedCount].id, VIDEO_ID_LEN, id);
            watched[watchedCount].position = position;
            watchedCount++;
         }
      }
   }
   free(buffer);
}

// a resumable key is a bare videoId; a typed url (carries '/' or ':') never enters history.
static int isHistoryKey(const char *videoId) { return videoId[0] && !strchr(videoId, '/') && !strchr(videoId, ':'); }

static WatchEntry *findWatched(const char *videoId)
{
   for (int i = 0; i < watchedCount; i++)
      if (strcmp(watched[i].id, videoId) == 0) return &watched[i];
   return NULL;
}

// add a fresh entry (ring-overwrites the oldest when full) at position 0.
static WatchEntry *addWatched(const char *videoId)
{
   WatchEntry *entry;
   if (watchedCount < HISTORY_CAP) entry = &watched[watchedCount++];
   else { entry = &watched[watchedNext]; watchedNext = (watchedNext + 1) % HISTORY_CAP; }
   strCopy(entry->id, VIDEO_ID_LEN, videoId);
   entry->position = 0;
   return entry;
}

// Seeded on first run only (the file is never overwritten afterwards, so this cannot undo anyone's list).
// The app ships subscribed to its author's channel and nothing else - a fresh install used to arrive
// following five strangers, which is not a sensible default for anybody.
// Format is "UCid<TAB>name", so the Subscriptions tab can label the tile before any network call.
static const char *DEFAULT_SUBS =
   "UCu3fQ5AKgUfJHFf2sZM9G3g\tTheErsysEnding\n";

static void seedSubscriptions(void)
{
   if (fileExists(subsPath)) return;
   if (writeFile(subsPath, DEFAULT_SUBS, strlen(DEFAULT_SUBS)) != 0)
      logError("[storage] could not seed %s\n", subsPath);
}

// ---- subscriptions ----
//
// Held in memory and mirrored to subscriptions.txt, exactly like the watch history and the watch-later
// queue. They used to be re-read from the file on EVERY call instead, which meant a single unreadable read
// looked like "no subscriptions" to every caller at once: isSubscribed could never see what setSubscribed
// had just written, so the button reported "Subscribe" for ever and appeared to do nothing at all.
static Subscription subs[MAX_SUBSCRIPTIONS];
static int          subsCount;
static int          subsRevision;   // bumped whenever the set in memory really changes

// one line is "UCxxxxxxxx" or "UCxxxxxxxx<TAB>Channel Name". The name is optional so files written by
// older builds (and hand-edited ones) still load.
static int parseSubscription(char *line, Subscription *out)
{
   if (line[0] != 'U' || line[1] != 'C') return 0;   // skips blanks and comments
   memset(out, 0, sizeof *out);
   char *tab = strchr(line, '\t');
   if (tab) { *tab = 0; strCopy(out->name, CHANNEL_NAME_LEN, tab + 1); }
   strCopy(out->id, CHANNEL_ID_LEN, line);
   return out->id[0] != 0;
}

static void loadSubscriptions(void)
{
   char *buffer = (char *)malloc(SUBS_BUFFER_BYTES);
   if (!buffer) { logError("[storage] subscriptions: no memory for the load buffer\n"); return; }

   int length = readFile(subsPath, buffer, SUBS_BUFFER_BYTES - 1);
   if (length < 0) {
      // the list stays empty, but subscribing still works for this session and writes through
      logError("[storage] subscriptions: %s could not be read (exists=%d)\n", subsPath, fileExists(subsPath));
   } else {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && subsCount < MAX_SUBSCRIPTIONS; line = strtok(NULL, "\r\n"))
         if (parseSubscription(line, &subs[subsCount])) subsCount++;
      logInfo("[storage] subscriptions: %d loaded (%d bytes)\n", subsCount, length);
   }
   free(buffer);
}

// Every save goes through here. Overwriting an EXISTING file can fail where creating a new one succeeds -
// a leftover the backend will not truncate - and that alone made the subscribe button look dead. Removing
// the file and writing it fresh is the fix, and it applies to history and preferences just as much.
static int saveFile(const char *path, const void *data, int length)
{
   if (writeFile(path, data, length) == 0) return 0;
   deleteFile(path);
   if (writeFile(path, data, length) != 0) return -1;
   logInfo("[storage] %s only took a write after being removed first\n", path);
   return 0;
}

static int saveSubscriptions(void)
{
   char *buffer = (char *)malloc(SUBS_BUFFER_BYTES);
   if (!buffer) return -1;
   int length = 0;
   for (int i = 0; i < subsCount; i++)
      length += snprintf(buffer + length, SUBS_LINE_MAX + 1, subs[i].name[0] ? "%s\t%s\n" : "%s\n",
                         subs[i].id, subs[i].name);
   int rc = saveFile(subsPath, buffer, length);
   free(buffer);
   if (rc != 0) {
      uint64_t freeBytes = 0;
      getFreeSpace(dataDir, &freeBytes, NULL);
      logError("[storage] subscriptions: writing %s FAILED (%d bytes, %llu KB free on the volume)\n",
               subsPath, length, (unsigned long long)(freeBytes / 1024));
   }
   return rc;
}

// ---- watch-later queue (full entries so the category renders offline) ----

static SearchResult watchLater[MAX_WATCHLATER];
static int          watchLaterCount;
static int          watchLaterRevision;

// split "videoId\tchannelId\ttitle\tduration\tauthor\tviews\tpublished\tisLive" into item. 1 ok, 0 malformed.
static int parseWatchLater(char *line, SearchResult *item)
{
   char *field[8];
   int count = 0;
   field[count++] = line;
   for (char *cursor = line; *cursor && count < 8; cursor++)
      if (*cursor == '\t') { *cursor = 0; field[count++] = cursor + 1; }
   if (count < 8) return 0;

   memset(item, 0, sizeof *item);
   strCopy(item->videoId,   sizeof item->videoId,   field[0]);
   strCopy(item->channelId, sizeof item->channelId, field[1]);
   strCopy(item->title,     sizeof item->title,     field[2]);
   strCopy(item->duration,  sizeof item->duration,  field[3]);
   strCopy(item->author,    sizeof item->author,    field[4]);
   strCopy(item->views,     sizeof item->views,     field[5]);
   strCopy(item->published, sizeof item->published, field[6]);
   item->isLive = atoi(field[7]);
   return item->videoId[0] != 0;
}

static void loadWatchLater(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(watchLaterPath, buffer, WATCHLATER_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && watchLaterCount < MAX_WATCHLATER; line = strtok(NULL, "\r\n"))
         if (parseWatchLater(line, &watchLater[watchLaterCount])) watchLaterCount++;
   }
   free(buffer);
}

static void saveWatchLater(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < watchLaterCount; i++) {
      const SearchResult *item = &watchLater[i];
      length += snprintf(buffer + length, WATCHLATER_LINE_MAX + 1, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                         item->videoId, item->channelId, item->title, item->duration,
                         item->author, item->views, item->published, item->isLive);
   }
   saveFile(watchLaterPath, buffer, length);
   free(buffer);
}

int getWatchLaterRevision(void) { return watchLaterRevision; }

// ---- watch history (full entries, so it can be browsed like any other feed) ----
//
// history.txt keeps ids and resume positions and is what powers the watched fade; this is the same set with
// the metadata needed to DRAW it, newest first. Kept separate so an old history.txt still loads.
static SearchResult historyList[MAX_WATCH_HISTORY];
static int          historyListCount;
static int          historyRevision;

static void loadWatchHistory(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(watchedPath, buffer, WATCHLATER_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && historyListCount < MAX_WATCH_HISTORY; line = strtok(NULL, "\r\n"))
         if (parseWatchLater(line, &historyList[historyListCount])) historyListCount++;
   }
   free(buffer);
}

static void saveWatchHistory(void)
{
   char *buffer = malloc(WATCHLATER_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < historyListCount; i++) {
      const SearchResult *item = &historyList[i];
      length += snprintf(buffer + length, WATCHLATER_LINE_MAX + 1, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                         item->videoId, item->channelId, item->title, item->duration,
                         item->author, item->views, item->published, item->isLive);
   }
   saveFile(watchedPath, buffer, length);
   free(buffer);
}

int getWatchHistoryRevision(void) { return historyRevision; }

void markWatchedItem(const SearchResult *item)
{
   markWatched(item->videoId);   // the id-only set: watched fade + resume position

   // rewatching moves the entry back to the front instead of adding a second one
   int count = historyListCount;
   for (int i = 0; i < count; i++)
      if (strcmp(historyList[i].videoId, item->videoId) == 0) {
         for (int j = i; j + 1 < count; j++) historyList[j] = historyList[j + 1];
         count--;
         break;
      }
   if (count == MAX_WATCH_HISTORY) count--;   // full: the oldest falls off the end
   for (int i = count; i > 0; i--) historyList[i] = historyList[i - 1];
   historyList[0] = *item;
   historyListCount = count + 1;
   historyRevision++;
   saveWatchHistory();
}

int getWatchHistory(SearchResults *out)
{
   memset(out, 0, sizeof *out);
   for (int i = 0; i < historyListCount && out->count < MAX_SEARCH_RESULTS; i++)
      out->items[out->count++] = historyList[i];
   return out->count;
}

void clearWatchHistory(void)
{
   historyListCount = 0;
   historyRevision++;
   saveWatchHistory();
}

// The membership rule is deliberately the PLAYER's resume rule, not "has a position". play.c only seeks
// when the position is past 3 s and more than 10 s from the end - so a video abandoned 3 seconds in, or one
// quit during the closing credits, would otherwise sit in this list for ever showing a full bar and
// restarting from zero every time it was picked.
#define RESUME_MIN_SECONDS  3
#define RESUME_END_MARGIN  10

int getContinueWatching(SearchResults *out)
{
   memset(out, 0, sizeof *out);
   for (int i = 0; i < historyListCount && out->count < MAX_SEARCH_RESULTS; i++) {
      const SearchResult *item = &historyList[i];
      if (item->isLive) continue;                       // a live stream has nothing to resume
      int position = getWatchedPosition(item->videoId);
      if (position <= RESUME_MIN_SECONDS) continue;
      int total = durationToSeconds(item->duration);    // 0 = unknown, which the player also treats as resumable
      if (total > 0 && position >= total - RESUME_END_MARGIN) continue;
      out->items[out->count++] = *item;
   }
   return out->count;
}

int isWatchLater(const char *videoId)
{
   for (int i = 0; i < watchLaterCount; i++)
      if (strcmp(watchLater[i].videoId, videoId) == 0) return 1;
   return 0;
}

void toggleWatchLater(const SearchResult *item)
{
   for (int i = 0; i < watchLaterCount; i++)
      if (strcmp(watchLater[i].videoId, item->videoId) == 0) {   // already queued: remove it
         for (int j = i; j < watchLaterCount - 1; j++) watchLater[j] = watchLater[j + 1];
         watchLaterCount--;
         saveWatchLater();
         watchLaterRevision++;
         return;
      }
   if (watchLaterCount >= MAX_WATCHLATER) return;
   watchLater[watchLaterCount++] = *item;   // add newest at the end
   saveWatchLater();
   watchLaterRevision++;
}

// fill out with the queued videos, newest first. returns the count.
int getWatchLater(SearchResults *out)
{
   memset(out, 0, sizeof *out);
   for (int i = watchLaterCount - 1; i >= 0 && out->count < MAX_SEARCH_RESULTS; i--)
      out->items[out->count++] = watchLater[i];
   return out->count;
}


// ---- playlists ----
//
// Held in RAM and mirrored to playlists.txt. The file is one "#Name" line per list followed by its videos in
// exactly the serialisation the watch-later queue uses, so there is a single entry format in this app rather
// than a second one to keep in step.
#define PLAYLIST_BUFFER_BYTES (MAX_PLAYLISTS * (PLAYLIST_NAME_LEN + 4 + MAX_PLAYLIST_ITEMS * WATCHLATER_LINE_MAX) + 64)

typedef struct {
   char         name[PLAYLIST_NAME_LEN];
   SearchResult items[MAX_PLAYLIST_ITEMS];
   int          count;
} Playlist;

static Playlist playlists[MAX_PLAYLISTS];
static int      playlistCount;
static int      playlistsRevision;

static void loadPlaylists(void)
{
   char *buffer = (char *)malloc(PLAYLIST_BUFFER_BYTES);
   if (!buffer) { logError("[storage] playlists: no memory for the load buffer\n"); return; }

   int length = readFile(playlistsPath, buffer, PLAYLIST_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      Playlist *current = NULL;
      for (char *line = strtok(buffer, "\r\n"); line; line = strtok(NULL, "\r\n")) {
         if (line[0] == '#') {
            if (playlistCount >= MAX_PLAYLISTS) { current = NULL; continue; }
            current = &playlists[playlistCount++];
            memset(current, 0, sizeof *current);
            strCopy(current->name, PLAYLIST_NAME_LEN, line + 1);
         } else if (current && current->count < MAX_PLAYLIST_ITEMS) {
            if (parseWatchLater(line, &current->items[current->count])) current->count++;
         }
      }
   }
   free(buffer);
   logInfo("[storage] playlists: %d loaded\n", playlistCount);
}

static void savePlaylists(void)
{
   char *buffer = (char *)malloc(PLAYLIST_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int p = 0; p < playlistCount; p++) {
      length += snprintf(buffer + length, PLAYLIST_NAME_LEN + 4, "#%s\n", playlists[p].name);
      for (int i = 0; i < playlists[p].count; i++) {
         const SearchResult *item = &playlists[p].items[i];
         length += snprintf(buffer + length, WATCHLATER_LINE_MAX + 1, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                            item->videoId, item->channelId, item->title, item->duration,
                            item->author, item->views, item->published, item->isLive);
      }
   }
   saveFile(playlistsPath, buffer, length);
   free(buffer);
}

static int validPlaylist(int playlist) { return playlist >= 0 && playlist < playlistCount; }

int getPlaylistCount(void) { return playlistCount; }
int getPlaylistsRevision(void) { return playlistsRevision; }

const char *getPlaylistName(int playlist) { return validPlaylist(playlist) ? playlists[playlist].name : ""; }
int getPlaylistItemCount(int playlist)    { return validPlaylist(playlist) ? playlists[playlist].count : 0; }

int getPlaylistItems(int playlist, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   if (!validPlaylist(playlist)) return 0;
   for (int i = 0; i < playlists[playlist].count && out->count < MAX_SEARCH_RESULTS; i++)
      out->items[out->count++] = playlists[playlist].items[i];
   return out->count;
}

int createPlaylist(const char *name)
{
   if (!name || !name[0]) return -1;
   for (int p = 0; p < playlistCount; p++)
      if (strcmp(playlists[p].name, name) == 0) return p;   // same name twice is the same list, not an error
   if (playlistCount >= MAX_PLAYLISTS) return -1;

   Playlist *list = &playlists[playlistCount];
   memset(list, 0, sizeof *list);
   strCopy(list->name, PLAYLIST_NAME_LEN, name);
   playlistCount++;
   playlistsRevision++;
   savePlaylists();
   return playlistCount - 1;
}

void deletePlaylist(int playlist)
{
   if (!validPlaylist(playlist)) return;
   for (int p = playlist; p + 1 < playlistCount; p++) playlists[p] = playlists[p + 1];
   playlistCount--;
   playlistsRevision++;
   savePlaylists();
}

void renamePlaylist(int playlist, const char *name)
{
   if (!validPlaylist(playlist) || !name || !name[0]) return;
   strCopy(playlists[playlist].name, PLAYLIST_NAME_LEN, name);
   playlistsRevision++;
   savePlaylists();
}

int isInPlaylist(int playlist, const char *videoId)
{
   if (!validPlaylist(playlist)) return 0;
   for (int i = 0; i < playlists[playlist].count; i++)
      if (strcmp(playlists[playlist].items[i].videoId, videoId) == 0) return 1;
   return 0;
}

int addToPlaylist(int playlist, const SearchResult *item)
{
   if (!validPlaylist(playlist) || !item->videoId[0]) return PLAYLIST_FULL;
   if (isInPlaylist(playlist, item->videoId)) return PLAYLIST_ALREADY;

   Playlist *list = &playlists[playlist];
   if (list->count >= MAX_PLAYLIST_ITEMS) return PLAYLIST_FULL;
   // newest first, like every other feed in this app
   for (int i = list->count; i > 0; i--) list->items[i] = list->items[i - 1];
   list->items[0] = *item;
   list->count++;
   playlistsRevision++;
   savePlaylists();
   return PLAYLIST_ADDED;
}

void removeFromPlaylist(int playlist, const char *videoId)
{
   if (!validPlaylist(playlist)) return;
   Playlist *list = &playlists[playlist];
   for (int i = 0; i < list->count; i++)
      if (strcmp(list->items[i].videoId, videoId) == 0) {
         for (int j = i; j + 1 < list->count; j++) list->items[j] = list->items[j + 1];
         list->count--;
         playlistsRevision++;
         savePlaylists();
         return;
      }
}

// ---- search history ----

// newest first, so the file reads in the order the list is drawn. Whole-file rewrite on every change: forty
// short lines is nothing, and it keeps the on-disk copy exact after each search.
static char searchHistory[MAX_SEARCH_HISTORY][SEARCH_QUERY_LEN];
static int  searchHistoryCount;

static void loadSearchHistory(void)
{
   char *buffer = malloc(SEARCH_HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(searchPath, buffer, SEARCH_HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      // queries contain spaces, so a line is one entry - never tokenise on whitespace here
      for (char *line = strtok(buffer, "\r\n"); line && searchHistoryCount < MAX_SEARCH_HISTORY; line = strtok(NULL, "\r\n"))
         if (line[0]) strCopy(searchHistory[searchHistoryCount++], SEARCH_QUERY_LEN, line);
   }
   free(buffer);
}

static void saveSearchHistory(void)
{
   char *buffer = malloc(SEARCH_HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < searchHistoryCount; i++)
      length += snprintf(buffer + length, SEARCH_HISTORY_BUFFER_BYTES - length, "%s\n", searchHistory[i]);
   saveFile(searchPath, buffer, length);
   free(buffer);
}

static int findSearchHistory(const char *query)
{
   for (int i = 0; i < searchHistoryCount; i++)
      if (strcmp(searchHistory[i], query) == 0) return i;
   return -1;
}

static void dropSearchHistoryAt(int index)
{
   for (int i = index; i + 1 < searchHistoryCount; i++)
      strCopy(searchHistory[i], SEARCH_QUERY_LEN, searchHistory[i + 1]);
   searchHistoryCount--;
}

int getSearchHistory(char out[][SEARCH_QUERY_LEN], int max)
{
   int count = searchHistoryCount < max ? searchHistoryCount : max;
   for (int i = 0; i < count; i++) strCopy(out[i], SEARCH_QUERY_LEN, searchHistory[i]);
   return count;
}

void addSearchHistory(const char *query)
{
   if (!query || !query[0]) return;

   int existing = findSearchHistory(query);
   if (existing >= 0) dropSearchHistoryAt(existing);                    // re-searching moves it to the front
   else if (searchHistoryCount == MAX_SEARCH_HISTORY) searchHistoryCount--;   // full: the oldest falls off

   for (int i = searchHistoryCount; i > 0; i--) strCopy(searchHistory[i], SEARCH_QUERY_LEN, searchHistory[i - 1]);
   strCopy(searchHistory[0], SEARCH_QUERY_LEN, query);
   searchHistoryCount++;
   saveSearchHistory();
}

void removeSearchHistory(const char *query)
{
   int index = findSearchHistory(query);
   if (index < 0) return;
   dropSearchHistoryAt(index);
   saveSearchHistory();
}

// ---- view preference ----

// one "name value" line per setting. Held in memory; the file is rewritten on every change (it is a handful
// of bytes, so there is nothing to batch). A setting missing from the file keeps its default here, so an
// older prefs.txt stays valid when a new setting is added.
static int autoplay = 1;

// Returns the value as WRITTEN, not as a boolean. It used to coerce with `atoi(...) ? 1 : 0`, which was
// invisible while the only settings were on/off flags - and then silently destroyed every one of them the
// moment a setting held a real number. skipmask/askmask are seven-bit category masks, so "skip sponsors,
// self-promo and interaction" (7) came back as 1 on the next launch and only sponsors survived; and the
// "defaults 2" marker came back as 1, so the once-per-install subscription reconcile ran on every boot.
// Callers that want a flag coerce it themselves at the call site.
static int readPref(const char *buffer, const char *name, int fallback)
{
   const char *found = strstr(buffer, name);
   return found ? atoi(found + strlen(name)) : fallback;
}

// -1 = "prefs.txt did not mention it", which is what triggers the one-time seed from settings.txt.
static int skipCategories = -1;
static int askCategories = 0;
// Which generation of the shipped subscription defaults this install has seen. Bumping it is what lets a
// change to DEFAULT_SUBS reach an EXISTING install: subscriptions.txt is never overwritten (rightly - it
// holds the user's own list), so without this a new default would only ever appear on a fresh install.
static int defaultsGeneration = 0;
#define DEFAULTS_GENERATION 2

#define SKIP_MASK_ALL ((1 << SPONSOR_CATEGORY_COUNT) - 1)
#define SKIP_MASK_ADS ((1 << SPONSOR_SPONSOR) | (1 << SPONSOR_SELFPROMO) | (1 << SPONSOR_INTERACTION))

// The old settings.txt mode was a single three-way switch; the screen offers each category separately, so
// the mode only decides where a fresh install starts.
static int skipMaskForMode(SponsorblockMode mode)
{
   if (mode == SPONSORBLOCK_OFF) return 0;
   if (mode == SPONSORBLOCK_ALL) return SKIP_MASK_ALL;
   return SKIP_MASK_ADS;
}

static void loadPrefs(void)
{
   char buffer[192];
   int length = readFile(prefsPath, buffer, sizeof buffer - 1);
   if (length > 0) {
      buffer[length] = 0;
      autoplay = readPref(buffer, "autoplay ", autoplay) ? 1 : 0;
      skipCategories = readPref(buffer, "skipmask ", skipCategories);
      askCategories  = readPref(buffer, "askmask ",  askCategories);
      defaultsGeneration = readPref(buffer, "defaults ", defaultsGeneration);
   }
   if (skipCategories < 0) {
      skipCategories = skipMaskForMode(getSponsorblockMode());
      logInfo("[storage] skip categories seeded from settings.txt: 0x%x\n", skipCategories);
   }
   skipCategories &= SKIP_MASK_ALL;   // a hand-edited file cannot set bits no category owns
   askCategories  &= SKIP_MASK_ALL;
   askCategories  &= ~skipCategories;   // skip wins: a category is never both
}

static void savePrefs(void)
{
   char buffer[192];
   int length = snprintf(buffer, sizeof buffer, "autoplay %d\nskipmask %d\naskmask %d\ndefaults %d\n",
                         autoplay, skipCategories, askCategories, defaultsGeneration);
   saveFile(prefsPath, buffer, length);
}

int getSkipCategories(void) { return skipCategories; }
int getAskCategories(void)  { return askCategories; }

int getSponsorAction(int category)
{
   if (category < 0 || category >= SPONSOR_CATEGORY_COUNT) return SPONSOR_ACTION_KEEP;
   if ((skipCategories >> category) & 1) return SPONSOR_ACTION_SKIP;
   if ((askCategories  >> category) & 1) return SPONSOR_ACTION_ASK;
   return SPONSOR_ACTION_KEEP;
}

void setSponsorAction(int category, int action)
{
   if (category < 0 || category >= SPONSOR_CATEGORY_COUNT) return;
   int bit = 1 << category;
   int skip = skipCategories & ~bit, ask = askCategories & ~bit;   // clear both, then set at most one
   if (action == SPONSOR_ACTION_SKIP) skip |= bit;
   else if (action == SPONSOR_ACTION_ASK) ask |= bit;
   if (skip == skipCategories && ask == askCategories) return;
   skipCategories = skip;
   askCategories = ask;
   savePrefs();
}

int getAutoplay(void) { return autoplay; }

void setAutoplay(int value)
{
   value = value ? 1 : 0;
   if (value == autoplay) return;
   autoplay = value;
   savePrefs();
}

// Files carried over from the old scratch directory. Copied one by one rather than as a whole tree, so the
// result is predictable no matter how copyTree treats its destination.
static const char *LEGACY_FILES[] = {
   "history.txt", "subscriptions.txt", "watchlater.txt", "prefs.txt", "searches.txt",
   "watched.txt", "settings.txt", "themes.txt", "visitor.txt", "playlists.txt",
};
#define MIGRATE_BUFFER_BYTES (64 * 1024)

// makeDir creates ONE level and openFs never creates parents, so a single missing directory anywhere in the
// path makes every write fail - which is exactly how the old /dev_hdd0/tmp/yo-player location broke once the
// console cleaned /dev_hdd0/tmp away. Create each component; only the leaf has to succeed (mkdir on an
// existing directory returns 0, and on a volume root it may legitimately refuse).
static int makeDirTree(const char *path)
{
   char partial[128];
   int length = (int)strlen(path);
   if (length <= 0 || length >= (int)sizeof partial) return -1;
   for (int i = 1; i <= length; i++) {
      if (path[i] != '/' && path[i] != '\0') continue;
      memcpy(partial, path, (size_t)i);
      partial[i] = '\0';
      int rc = makeDir(partial);
      if (rc != 0 && i == length) return -1;
   }
   return 0;
}

// set once at startup by migrateAppData: 1 only if the data directory exists AND accepted a real write.
static int dataDirWritable;

const char *getSubscriptionsPath(void) { return subsPath; }

// free space on the volume the data directory lives on, in KB. For error messages: a write that fails on a
// full disk and a write that fails for any other reason need completely different answers.
unsigned long getStorageFreeKB(void)
{
   uint64_t freeBytes = 0;
   if (getFreeSpace(dataDir, &freeBytes, NULL) != 0) return 0;
   return (unsigned long)(freeBytes / 1024);
}
int isStorageWritable(void) { return dataDirWritable; }

// In preference order. The first one that can be created AND takes a real write wins.
//   1. outside the game folder, so uninstalling the package to install a newer build keeps everything
//   2. where the app used to keep it (and where the rest of ps3-dev still does)
//   3. the app's own folder - always writable, but the console deletes it when the package is removed
static const char *CANDIDATE_DIRS[] = {
   YO_DATA_DIR,                             // survives uninstall and reboot
   "/dev_hdd0/home/00000001/yo-player",     // the user profile; also survives both
   "/dev_hdd0/game/YOPLAYER1/USRDIR/data",  // always writable, but the console deletes it on uninstall
   YO_LEGACY_DIR,                           // last resort: works, but the console clears it
};

// creates `dir` and proves it accepts a write. 0 if usable.
static int dataDirUsable(const char *dir)
{
   // report WHICH half fails: "cannot create the folder" and "folder exists but refuses a file" need
   // completely different answers, and lumping them together cost several rounds of guessing.
   if (makeDirTree(dir) != 0) { logError("[storage] %s: cannot create the directory\n", dir); return -1; }
   char probe[112];
   snprintf(probe, sizeof probe, "%s/.writetest", dir);
   if (writeFile(probe, "ok\n", 3) != 0) { logError("[storage] %s: created, but will not take a file\n", dir); return -1; }
   deleteFile(probe);
   return 0;
}

void migrateAppData(void)
{
   useDataDir(CANDIDATE_DIRS[0]);   // so the getters return something sensible even if all of them fail

   int chosen = -1;
   for (unsigned i = 0; i < sizeof CANDIDATE_DIRS / sizeof CANDIDATE_DIRS[0]; i++) {
      if (dataDirUsable(CANDIDATE_DIRS[i]) != 0) continue;
      chosen = (int)i;
      break;
   }
   if (chosen < 0) {
      logError("[storage] NO writable data directory found - nothing will be saved\n");
      return;
   }

   useDataDir(CANDIDATE_DIRS[chosen]);
   dataDirWritable = 1;
   uint64_t freeBytes = 0, totalBytes = 0;
   getFreeSpace(dataDir, &freeBytes, &totalBytes);
   logInfo("[storage] data directory: %s (candidate %d), %llu MB free of %llu MB\n", dataDir, chosen,
           (unsigned long long)(freeBytes / (1024 * 1024)), (unsigned long long)(totalBytes / (1024 * 1024)));

   if (fileExists(migrateMarker)) return;   // already done on an earlier run

   // bring across whatever an older build left in the other candidates
   void *buffer = malloc(MIGRATE_BUFFER_BYTES);
   if (!buffer) return;
   int moved = 0;
   for (unsigned d = 0; d < sizeof CANDIDATE_DIRS / sizeof CANDIDATE_DIRS[0]; d++) {
      if ((int)d == chosen) continue;
      for (unsigned i = 0; i < sizeof LEGACY_FILES / sizeof LEGACY_FILES[0]; i++) {
         char from[128], to[128];
         snprintf(from, sizeof from, "%s/%s", CANDIDATE_DIRS[d], LEGACY_FILES[i]);
         snprintf(to,   sizeof to,   "%s/%s", dataDir,            LEGACY_FILES[i]);
         if (!fileExists(from) || fileExists(to)) continue;
         if (copyFile(from, to, buffer, MIGRATE_BUFFER_BYTES) == 0) moved++;
      }
   }
   free(buffer);
   writeFile(migrateMarker, "1\n", 2);
   if (moved) logInfo("[storage] carried %d file(s) over into %s\n", moved, dataDir);
}

// The five channels this app used to ship subscribed to. They were never chosen by anyone - they came with
// the first launch - so when the shipped default changed, an existing install was left following strangers
// with no way out but editing subscriptions.txt over FTP.
//
// This removes exactly those five ids and nothing else: a channel the user subscribed to themselves is
// untouched even if it happens to be one of them again, because it would have to be re-added by hand after
// this has run once. Runs once per install, guarded by defaultsGeneration.
static const char *RETIRED_DEFAULT_SUBS[] = {
   "UCX6OQ3DkcsbYNE6H8uQQuVA",   // MrBeast
   "UCXuqSBlHAE6Xw-yeJA0Tunw",   // Linus Tech Tips
   "UCBJycsmduvYEL83R_U4JriQ",   // MKBHD
   "UCHnyfMqiRRG1u-2MsSQLbXA",   // Veritasium
   "UCsXVk37bltHxD1rDPwtNM8Q",   // Kurzgesagt
};
#define RETIRED_DEFAULT_COUNT ((int)(sizeof RETIRED_DEFAULT_SUBS / sizeof RETIRED_DEFAULT_SUBS[0]))

#define OWN_CHANNEL_ID   "UCu3fQ5AKgUfJHFf2sZM9G3g"
#define OWN_CHANNEL_NAME "TheErsysEnding"

static void reconcileDefaultSubscriptions(void)
{
   if (defaultsGeneration >= DEFAULTS_GENERATION) return;

   int removed = 0, out = 0;
   for (int i = 0; i < subsCount; i++) {
      int retired = 0;
      for (int r = 0; r < RETIRED_DEFAULT_COUNT; r++)
         if (strcmp(subs[i].id, RETIRED_DEFAULT_SUBS[r]) == 0) { retired = 1; break; }
      if (retired) { removed++; continue; }
      subs[out++] = subs[i];
   }
   subsCount = out;

   int hasOwn = 0;
   for (int i = 0; i < subsCount; i++)
      if (strcmp(subs[i].id, OWN_CHANNEL_ID) == 0) { hasOwn = 1; break; }
   if (!hasOwn && subsCount < MAX_SUBSCRIPTIONS) {
      memset(&subs[subsCount], 0, sizeof subs[subsCount]);
      strCopy(subs[subsCount].id, CHANNEL_ID_LEN, OWN_CHANNEL_ID);
      strCopy(subs[subsCount].name, CHANNEL_NAME_LEN, OWN_CHANNEL_NAME);
      subsCount++;
   }

   defaultsGeneration = DEFAULTS_GENERATION;
   savePrefs();                       // record it even if the list write fails, so this cannot loop forever
   if (removed || !hasOwn) {
      subsRevision++;                 // the subscriptions feed on screen is now stale
      if (saveSubscriptions() != 0) logError("[storage] could not write the reconciled subscription list\n");
      else logInfo("[storage] subscriptions: dropped %d shipped default(s), own channel %s\n",
                   removed, hasOwn ? "already present" : "added");
   }
}

void initStorage(void)
{
   // migrateAppData() has already chosen and verified the directory
   loadHistory();
   seedSubscriptions();
   loadSubscriptions();
   loadWatchLater();
   loadWatchHistory();
   loadPrefs();                       // before the reconcile: it carries the generation marker
   reconcileDefaultSubscriptions();
   loadPlaylists();
   loadSearchHistory();
}

int getSubscriptionList(Subscription *out, int max)
{
   int count = subsCount < max ? subsCount : max;
   for (int i = 0; i < count; i++) out[i] = subs[i];
   return count;
}

int getSubscriptions(char ids[][CHANNEL_ID_LEN], int max)
{
   int count = subsCount < max ? subsCount : max;
   for (int i = 0; i < count; i++) strCopy(ids[i], CHANNEL_ID_LEN, subs[i].id);
   return count;
}

int getSubscriptionsRevision(void) { return subsRevision; }

int isSubscribed(const char *channelId)
{
   for (int i = 0; i < subsCount; i++)
      if (strcmp(subs[i].id, channelId) == 0) return 1;
   return 0;
}

int setSubscribed(const char *channelId, const char *channelName, int subscribed)
{
   if (channelId[0] != 'U' || channelId[1] != 'C') return SUBSCRIBE_ERR_ID;   // only real UC ids

   int found = -1;
   for (int i = 0; i < subsCount; i++)
      if (strcmp(subs[i].id, channelId) == 0) { found = i; break; }

   if (subscribed && found < 0) {
      if (subsCount >= MAX_SUBSCRIPTIONS) return SUBSCRIBE_ERR_FULL;
      memset(&subs[subsCount], 0, sizeof subs[subsCount]);
      strCopy(subs[subsCount].id, CHANNEL_ID_LEN, channelId);
      if (channelName) strCopy(subs[subsCount].name, CHANNEL_NAME_LEN, channelName);
      subsCount++;
   } else if (subscribed && found >= 0) {
      // already subscribed: the only thing left worth doing is filling in a name we did not have
      if (!channelName || !channelName[0] || subs[found].name[0]) return SUBSCRIBE_OK;
      strCopy(subs[found].name, CHANNEL_NAME_LEN, channelName);
   } else if (!subscribed && found >= 0) {
      for (int i = found; i + 1 < subsCount; i++) subs[i] = subs[i + 1];
      subsCount--;
   } else {
      return SUBSCRIBE_OK;   // already in the desired state
   }

   // the set in memory really did change, so the feeds must refresh whatever the disk says next
   subsRevision++;

   // the write is best-effort: the change is already live for this session, so a failed write costs
   // persistence across a restart, not the feature itself - but the user is told about it.
   return saveSubscriptions() == 0 ? SUBSCRIBE_OK : SUBSCRIBE_ERR_WRITE;
}

int isWatched(const char *videoId) { return findWatched(videoId) != NULL; }

int getWatchedPosition(const char *videoId)
{
   WatchEntry *entry = findWatched(videoId);
   return entry ? entry->position : 0;
}

// rewrite history.txt from the in-memory set (one "id position" per line).
static void saveHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < watchedCount; i++)
      length += snprintf(buffer + length, ENTRY_LINE_MAX + 1, "%s %d\n", watched[i].id, watched[i].position);
   saveFile(historyPath, buffer, length);
   free(buffer);
}

void markWatched(const char *videoId)
{
   if (!isWatched(videoId)) setWatchedPosition(videoId, 0);   // add new at position 0; keep an existing position
}

void setWatchedPosition(const char *videoId, int seconds)
{
   if (!isHistoryKey(videoId)) return;
   WatchEntry *entry = findWatched(videoId);
   if (!entry) entry = addWatched(videoId);
   entry->position = seconds;
   // the "Continue watching" feed is built from these positions, so a changed position makes a cached one
   // stale exactly like a new history entry does. Cheap: the revision is only read when a screen resumes.
   historyRevision++;
   saveHistory();
}

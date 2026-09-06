#pragma once

// storage - tiny persistent preferences + watch history, kept as plain text files under
// /dev_hdd0/tmp/yo-player/ (via the VFS layer). No user/account binding: back up or move the folder and
// anyone can restore it. initStorage() must run once at startup before the getters are used.

#include "extractor.h"   // SearchResult / SearchResults (watch-later stores full entries)

void initStorage(void);

// watch history: videoIds the user has played. Used to fade already-watched tiles in the grid.
int  isWatched(const char *videoId);
void markWatched(const char *videoId);

// the same history with the metadata needed to draw it, newest first - so "what did I watch" is a browsable
// list rather than just a dimmed tile. markWatchedItem records both; markWatched alone (an id with no entry,
// e.g. an autoplay hand-over) still fades the tile and keeps the resume position.
#define MAX_WATCH_HISTORY 200
void markWatchedItem(const SearchResult *item);
int  getWatchHistory(SearchResults *out);
int  getWatchHistoryRevision(void);
void clearWatchHistory(void);

// "Continue watching": the history entries that were left part-way through, newest first. A video played to
// the end is stored at position 0 (see setWatchedPosition), so "has a position" already means "unfinished" -
// no extra bookkeeping. Shares getWatchHistoryRevision(), which setWatchedPosition also bumps.
int  getContinueWatching(SearchResults *out);

// resume: last playback position (seconds) per watched video, so playback picks up where it left off.
// getWatchedPosition returns 0 if unknown; a finished video is saved as 0 so it restarts next time.
int  getWatchedPosition(const char *videoId);
void setWatchedPosition(const char *videoId, int seconds);

#define CHANNEL_ID_LEN    32
#define CHANNEL_NAME_LEN  48
#define MAX_SUBSCRIPTIONS 128

// one subscribed channel. The name is remembered at subscribe time so the subscriptions screen can list the
// channels without a network round trip per entry - and can still list them when a fetch fails. Entries
// written by older builds carry no name; those fall back to the id until the channel is opened again.
typedef struct {
   char id[CHANNEL_ID_LEN];
   char name[CHANNEL_NAME_LEN];
} Subscription;

// subscribed channels, seeded to subscriptions.txt on first run. Both fill out[0..count-1]; getSubscriptions
// is the id-only form for callers that don't need the names. getSubscriptionList returns the count, or -1 if
// the file could not be read at all - which is deliberately distinct from 0 ("no subscriptions"), because a
// caller that rewrites the file must not treat an unreadable file as an empty one.
int  getSubscriptionList(Subscription *out, int max);
int  getSubscriptions(char ids[][CHANNEL_ID_LEN], int max);

// subscribe toggle: isSubscribed reports the current state; setSubscribed adds or removes the channel and
// rewrites subscriptions.txt immediately. Only real "UC..." ids are stored; duplicates never occur.
// channelName may be NULL or empty (an existing entry then keeps the name it already had).
//
// setSubscribed REPORTS what happened. It used to be void, which meant every failure - an unreadable file,
// a failed write, a full list - looked exactly like success to the caller, and the user saw a button that
// did nothing. Callers are expected to tell the user which of these it was.
#define SUBSCRIBE_OK         0
#define SUBSCRIBE_ERR_ID    (-1)   // not a real "UC..." id, so there is nothing to subscribe to
#define SUBSCRIBE_ERR_READ  (-2)   // subscriptions.txt unreadable; nothing was changed
#define SUBSCRIBE_ERR_WRITE (-3)   // the new list could not be written back
#define SUBSCRIBE_ERR_FULL  (-4)   // already at MAX_SUBSCRIPTIONS

// where subscriptions are saved, and whether the data directory actually took a write at startup. Both
// exist so an error can NAME the file and the folder instead of leaving the user (and me) guessing which
// build and which location a failure came from.
const char *getSubscriptionsPath(void);
int  isStorageWritable(void);
unsigned long getStorageFreeKB(void);   // free space on the data volume, for "is the disk full?" messages

int  isSubscribed(const char *channelId);
int  setSubscribed(const char *channelId, const char *channelName, int subscribed);

// bumped whenever setSubscribed actually changes the set. Screens that cache the subscriptions feed compare
// this against the value they last loaded at, and refetch when it moved.
int  getSubscriptionsRevision(void);

// watch-later: a saved queue of full video entries (watchlater.txt), also shown as its own home category.
// isWatchLater powers the tile badge; toggleWatchLater queues the video (its metadata is kept so the
// category renders without a re-fetch) or removes it if already queued; getWatchLater fills a feed
// newest-first. getWatchLaterRevision bumps on every real change so cached feeds know to refetch.
int  isWatchLater(const char *videoId);
void toggleWatchLater(const SearchResult *item);
int  getWatchLater(SearchResults *out);
int  getWatchLaterRevision(void);

// search history: the queries entered most recently, newest first. The system keyboard keeps no memory of
// its own - cellOskDialog exposes no option for it, which is why typing in this app never offers what you
// typed last time - so the app remembers the queries itself and offers them as a pickable list.
#define MAX_SEARCH_HISTORY 40
#define SEARCH_QUERY_LEN   128

int  getSearchHistory(char out[][SEARCH_QUERY_LEN], int max);   // fills newest first, returns how many
void addSearchHistory(const char *query);                       // a repeat query moves back to the front
void removeSearchHistory(const char *query);

// What to do with each SponsorBlock category (see sponsorblock.h). Three choices, because "skip it" and
// "ignore it" are not the only sensible answers: ASK leaves the decision to the moment, showing a button
// during the segment - the way YouTube's own skip button works - which is what you want for intros, where
// sometimes you do want to watch them.
//
// Kept in prefs.txt as two bitmasks (skipmask/askmask); a category is in at most one of them. Seeded on
// first run from settings.txt's sponsorblock-mode, after which the settings screen owns it.
#define SPONSOR_ACTION_KEEP 0
#define SPONSOR_ACTION_ASK  1
#define SPONSOR_ACTION_SKIP 2

int  getSponsorAction(int category);
void setSponsorAction(int category, int action);

// the raw masks, for the cheap "is SponsorBlock worth fetching at all" test
int  getSkipCategories(void);
int  getAskCategories(void);

// autoplay: when a video ends, roll on to the next entry of the list it was started from. On by default.
int  getAutoplay(void);
void setAutoplay(int autoplay);

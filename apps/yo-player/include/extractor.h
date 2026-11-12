#pragma once

// extractor - source-agnostic media extraction interface (yt-dlp / NewPipe
// style). Each site (youtube, and later others) implements one Extractor. The
// player never knows which site a stream came from; it just gets a StreamInfo.

#define MAX_STREAM_URL     2048
#define MAX_STREAM_FORMATS 24
#define MAX_AUDIO_TRACKS   32

// one selectable stream. url may be empty if the source needs deobfuscation we
// haven't done yet (e.g. youtube's cipher) -- formatCount still reflects what
// the source offered.
typedef struct {
   int  itag;              // source-specific format id (0 if n/a)
   int  width, height;
   int  fps;               // frames per second (0 if the source doesn't say)
   int  hasVideo, hasAudio;
   int  needsCipher;       // 1 = url is signature-ciphered, not yet playable
   char container[8];      // "mp4"
   char url[MAX_STREAM_URL];
   int  isLiveSegmented;   // url is a DASH segment base: fetch "<url>sq/<n>/..." instead of opening it directly
   long liveStartSq;       // earliest segment number available (DVR window start)
   long liveEdgeSq;        // most recent segment number available (live edge)
   int  isDrc;             // audio: the dynamic-range-compressed copy YouTube serves beside the normal mix
   int  isVp9;             // video: VP9 profile 0 in WebM - 1440p for the TEE VP9 building block (V3.0.0)
   int  isOpus;            // audio: Opus in WebM (itag 251) - VP9's sound, on the TEE Opus building block (V3.0.0)
} StreamFormat;

// How much an audio track is worth playing by default, lowest first. YouTube serves some videos with several
// audio tracks - the uploader's own other languages, AI auto-dubs (rolled out from 2025) and audio
// description - and the track an English-language request is told is the default is often an auto-dub
// rather than what was recorded. The original wins; an auto-dub only plays when nothing better is offered.
enum { AUDIO_RANK_DESCRIPTIVE, AUDIO_RANK_DUBBED, AUDIO_RANK_OTHER, AUDIO_RANK_DEFAULT, AUDIO_RANK_ORIGINAL };

// one audio track (language) of a video that has several. url is that track's best AAC stream.
typedef struct {
   char id[24];            // "en-US.4", or language + kind from the url's xtags when the reply names none
   char name[64];          // "English (United States) original", "German (auto-dubbed)" - shown on screen
   int  rank;              // AUDIO_RANK_*
   int  itag;
   int  isDrc;
   char url[MAX_STREAM_URL];
} AudioTrack;

#define MAX_CAPTION_TRACKS 8

// one subtitle track offered by the source. url fetches the timed-text file (see subtitles.h).
typedef struct {
   char languageCode[12];   // "en", "es-419"
   char name[48];           // display name, e.g. "English (auto-generated)"
   char url[MAX_STREAM_URL];
} CaptionTrack;

typedef struct {
   char title[256];
   char description[2048];   // video description, truncated; newlines are real '\n'
   int  captionCount;
   CaptionTrack captions[MAX_CAPTION_TRACKS];
   int  durationSeconds;
   int  isLive;            // currently broadcasting: streams need live segment-sequence fetching, not a static moov
   int  isPostLive;        // a live stream that just ended (isPostLiveDvr): YouTube serves its recording as the same
                           // sq segments, the last one fixed, until it has made a video of it - no static moov either
   // a live stream's rewind (YouTube's DVR): whether the streamer left it on (isLiveDvrEnabled), how long each
   // sq segment is (targetDurationSec: 5, or 2 for low latency) and how far back it reaches (maxDvrDurationSec,
   // 12 h). 0 where the reply does not say.
   int  liveDvr;
   int  liveSegmentSec;
   int  liveWindowSec;
   int  formatCount;
   StreamFormat formats[MAX_STREAM_FORMATS];
   // a video with several audio tracks: all of them, best first (see AudioTrack). formats[] then holds only
   // audioTracks[0]'s stream, so playback and downloads pick it like any single audio stream - and two dozen
   // dubs (two or three AAC streams each) can no longer push the original past the end of formats[]. 0 for
   // the usual one-track video, whose audio is in formats[] as before.
   int  audioTrackCount;
   AudioTrack audioTracks[MAX_AUDIO_TRACKS];
} StreamInfo;

// accumulated across pages (infinite scroll); one page is ~20. only the ~30 on-screen tiles hold a
// thumbnail texture + rasterised labels at once (windowed), so this bounds metadata + kept jpegs, not VRAM.
#define MAX_SEARCH_RESULTS 500

// one search hit. videoId is what playVideo() / extract() take; channelId enters that video's channel feed.
typedef struct {
   char videoId[16];
   char channelId[32];     // owning channel's UC id (for "open channel"); empty if not parsed
   char title[160];
   char duration[12];      // "3:15" (empty if unknown / live)
   char author[48];        // channel name
   char views[24];         // "6,100 views"
   char published[24];     // "1 hour ago"
   int  isLive;            // 1 = live now (no duration; shown as a red LIVE badge)
} SearchResult;

// "3:15" / "1:02:33" -> seconds. 0 for anything that is not a plain duration (live, or unknown), so a
// caller can just skip a zero. Tiles only ever carry the formatted string, never a second count - and both
// the progress line and the "Continue watching" feed need the number, so it lives with the field.
static inline int durationToSeconds(const char *text)
{
   int parts[3] = {0, 0, 0}, count = 0, value = 0, digits = 0;
   for (const char *p = text; ; p++) {
      if (*p >= '0' && *p <= '9') {
         if (value > 100000) return 0;            // absurd field: refuse rather than overflow
         value = value * 10 + (*p - '0'); digits++; continue;
      }
      if (*p != ':' && *p != 0) return 0;         // stray character: not a duration
      if (!digits || count == 3) return 0;        // empty field, or more parts than h:mm:ss
      parts[count++] = value;
      if (*p == 0) break;
      value = 0; digits = 0;
   }
   if (count == 1) return parts[0];
   if (count == 2) return parts[0] * 60 + parts[1];
   return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

#define MAX_CONTINUATION 4096   // youtube "load more" tokens are long base64 blobs
#define MAX_SORT_TOKEN   512    // a channel sort chip's continuation token

typedef struct {
   int          count;
   char         continuation[MAX_CONTINUATION];   // token for the next page (empty = no more)
   char         channelName[48];                  // viewed channel's title (channel feeds only; else empty)
   // channel Videos feeds carry three sort chips (Latest/Popular/Oldest); each is a continuation token the
   // screen re-browses with. empty outside channel mode. index matches ChannelSort.
   char         sortTokens[3][MAX_SORT_TOKEN];
   SearchResult items[MAX_SEARCH_RESULTS];
} SearchResults;

// trending categories (YouTube retired the single aggregated feed; these are the surviving per-category
// channel-tab feeds). the UI cycles them; keep TrendingNames (home.c) and TRENDING_FEEDS (youtube.c) in sync.
typedef enum {
   TREND_GAMING,
   TREND_SPORTS,
   TREND_PODCASTS,
   TREND_COUNT
} TrendingCategory;

// the Live page's chips, in chip order: live streams only. keep LiveNames (home.c) and LIVE_FEEDS (youtube.c)
// in sync with this order.
typedef enum { LIVE_POPULAR, LIVE_GAMING, LIVE_SPORTS, LIVE_PODCASTS, LIVE_NEWS, LIVE_MUSIC, LIVE_COUNT } LiveCategory;

// search result ordering. YouTube's newer WEB API only offers these two sorts (its "Prioritize" menu
// dropped date-sort); the UI cycles them with SELECT. keep SortNames in the search screen in sync.
typedef enum {
   SORT_RELEVANCE,
   SORT_VIEWS,
   SORT_COUNT
} SortOrder;

// a channel Videos feed's sort chips. index matches SearchResults.sortTokens. keep ChannelSortNames in
// the search screen in sync with this order.
typedef enum {
   CHANNEL_SORT_LATEST,
   CHANNEL_SORT_POPULAR,
   CHANNEL_SORT_OLDEST,
   CHANNEL_SORT_COUNT
} ChannelSort;

// The signed-in account's own lists (account.h). Keep ACCOUNT_BROWSE_IDS (youtube.c) in this order.
typedef enum {
   ACCOUNT_FEED_HOME,            // the recommendations ("For you")
   ACCOUNT_FEED_SUBSCRIPTIONS,   // the newest uploads of the channels the account follows
   ACCOUNT_FEED_HISTORY,
   ACCOUNT_FEED_WATCH_LATER,
   ACCOUNT_FEED_CHANNELS,        // the channels it follows, one item each: channelId and title (the name), no video
   ACCOUNT_FEED_COUNT
} AccountFeed;

// one of the signed-in account's playlists, as the Playlists page lists it
#define ACCOUNT_PLAYLIST_ID_CAP 64
typedef struct {
   char id[ACCOUNT_PLAYLIST_ID_CAP];   // "LL" for Liked videos, else the playlist's own id
   char title[96];
   char count[24];                     // as YouTube words it: "48 videos"
} AccountPlaylistInfo;

// a change to the signed-in account (account.c tells YouTube about them on a worker)
typedef enum {
   ACCOUNT_ADD_WATCH_LATER,      // id: a video
   ACCOUNT_REMOVE_WATCH_LATER,
   ACCOUNT_SUBSCRIBE,            // id: a channel (UC...)
   ACCOUNT_UNSUBSCRIBE,
} AccountAction;

typedef struct Extractor {
   const char *name;                                    // "youtube"
   int (*matches)(const char *input);                   // claim this url/id?
   int (*extract)(const char *input, StreamInfo *out);  // 0 ok, negative on error
   // continuation is NULL for the first page, or a prior result's token to fetch the next page.
   int (*search)(const char *query, SortOrder sort, const char *continuation, SearchResults *out);
   int (*getTrending)(TrendingCategory category, const char *continuation, SearchResults *out);
   // a channel's Videos feed. token NULL = first page (Latest); else a sort-chip or next-page token.
   int (*getChannel)(const char *channelId, const char *token, SearchResults *out);
   // live streams only (the Live page). continuation NULL = page 1, else a prior page's token passed back exactly
   // as it came: a live token can carry the session its feed belongs to. stopped may be NULL; when it returns
   // non-zero the call gives up before a second request. Last on purpose: youtube.c fills this struct
   // positionally, so a member in the middle would shift every pointer after it.
   int (*getLive)(LiveCategory category, const char *continuation, SearchResults *out, int (*stopped)(void));
   // the signed-in account's lists (account.h); continuation as above. Appended for the same reason as getLive.
   int (*getAccount)(AccountFeed feed, const char *continuation, SearchResults *out);
   int (*changeAccount)(AccountAction action, const char *id);   // blocks; 0 when YouTube took it
   int (*getAccountPlaylists)(AccountPlaylistInfo *out, int cap);
   int (*getAccountPlaylist)(const char *playlistId, const char *continuation, SearchResults *out);
} Extractor;

// picks the first registered extractor that claims input, or NULL.
const Extractor *findExtractor(const char *input);

// text search. continuation NULL = first page, else a prior result's token. 0 ok, negative on error.
int searchVideos(const char *query, SortOrder sort, const char *continuation, SearchResults *out);

// a trending category feed (the launch/home screen). continuation as above. 0 ok, negative on error.
int getTrending(TrendingCategory category, const char *continuation, SearchResults *out);

// a Live page feed: live streams only, from the content region (settings.h). continuation NULL = page 1, else
// a prior result's token, passed back untouched. stopped (may be NULL) is asked before a second request, so a
// page switched away from does not wait for it. 0 ok (also with no items), negative on a transport/HTTP error.
int getLiveFeed(LiveCategory category, const char *continuation, SearchResults *out, int (*stopped)(void));

// a channel's Videos feed. token NULL = first page; else a sort-chip token (SearchResults.sortTokens) or a
// next-page continuation. the first page also fills out->sortTokens. 0 ok, negative on error.
int getChannelVideos(const char *channelId, const char *token, SearchResults *out);

// one of the signed-in account's lists, as videos (Shorts, channels, playlists and ads left out). continuation NULL
// = page 1, else a prior result's token. 0 ok (also with no items); negative when not signed in, or on a
// transport/HTTP error.
int getAccountFeed(AccountFeed feed, const char *continuation, SearchResults *out);

// one change to the signed-in account, sent at once (blocks on the network). 0 when YouTube took it.
int changeAccount(AccountAction action, const char *id);

// The signed-in account's playlists - Liked videos and its own, Watch Later left out (it has a page of its own).
// Blocks on the network. The number found, or negative on an error.
int getAccountPlaylists(AccountPlaylistInfo *out, int cap);

// one of them as a feed of videos; continuation as for the other feeds
int getAccountPlaylistVideos(const char *playlistId, const char *continuation, SearchResults *out);

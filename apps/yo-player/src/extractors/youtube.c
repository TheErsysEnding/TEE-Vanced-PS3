// youtube extractor - InnerTube player request + parse.
//
// POSTs the player endpoint as the VISIONOS (Apple Vision Pro) client, which returns
// direct (unciphered) urls and - unlike plain ANDROID/WEB - is not behind the
// GVS PoToken wall, so its adaptive (fragmented) urls above 360p don't 403.
// Those streams are fragmented mp4 (moof); the demuxer handles them.
//
// Fragile by nature: Google keeps closing these token-exempt clients. ANDROID_VR
// (Oculus) was capped to ~60 seconds of media on 2026-08-26; VISIONOS replaced it
// and serves full-length urls. Expect this to need swapping again in future.

#include "extractor.h"
#include "app-paths.h"
#include "settings.h"           // getContentRegionCode: the gl of every list request
#include "account.h"            // the signed-in account's lists: its token, and YouTube's TV client they go out as
#include "http.h"               // fetchHttp
#include "string-utilities.h"   // strCopy
#include "dbg.h"                // logInfo (resolve diagnostics)
#include "vfs.h"                // readFile/writeFile/deleteFile (persist the visitor session)

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PLAYER_URL "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"
#define SEARCH_URL "https://www.youtube.com/youtubei/v1/search?prettyPrint=false"
#define BROWSE_URL "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false"
#define RESP_CAP   (2 * 1024 * 1024)   // player JSON can pass 1 MB; a truncated response loses formats (and their urls)

// two clients, each for what it does best:
//  - VISIONOS (playback), see below. WEB/plain ANDROID are PoToken-gated for
//    adaptive (their googlevideo urls 403 above 360p - only progressive itag 18
//    is token-exempt); TVHTML5 hit the bot wall; iOS gave only fragmented adaptive.
//  - VISIONOS (Apple Vision Pro) is currently exempt from the GVS PO-token wall and
//    returns direct (un-ciphered) adaptive urls that serve full-length, so it plays
//    above 360p past the ~60s cap that now hits ANDROID_VR (the client it replaced).
//  - WEB (search): search isn't PoToken-gated, and WEB reliably returns the
//    videoRenderer result structure we parse.
#define PLAYER_VERSION "1.02"
#define WEB_VERSION    "2.20250701.00.00"

// visitor session endpoint: mints an anonymous visitorData that clears the
// logged-out LOGIN_REQUIRED ("confirm you're not a bot") wall (see extract()).
// the key below is youtube's own public web key, shipped in every youtube.com page. not a credential.
#define VISITOR_URL "https://www.youtube.com/youtubei/v1/visitor_id?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false"

// the VISIONOS client object, without the wrapping braces so visitorData can
// be appended before the player request closes it. Its gl stays "US" whatever the content region: the player
// answered alike for every gl probed, and this is the one request that decides whether anything plays.
#define PLAYER_CLIENT \
   "\"clientName\":\"VISIONOS\",\"clientVersion\":\"" PLAYER_VERSION "\"," \
   "\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\"," \
   "\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\",\"hl\":\"en\",\"gl\":\"US\""
// The WEB client of every list request: browse, search and their next pages. hl stays "en" - the parsers key on
// English text (" views", " ago", " watching", "Members only"). gl is the content region (settings.h): every
// format built on this takes it as its FIRST argument, and a request reads getContentRegionCode() once and passes
// that one value. The root object is left open for each body's own fields.
#define WEB_CONTEXT_FMT \
   "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"%s\"}}"

static const char *PLAYER_UA =
   "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
static const char *WEB_UA =
   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

// player body: the client + the minted visitorData, then the video id. two %s: visitorData, videoId.
static const char *PLAYER_BODY_FMT =
   "{\"context\":{\"client\":{" PLAYER_CLIENT ",\"visitorData\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
// The visitor-session mint keeps its own fixed client, byte for byte what it always sent: the session it returns
// records the IP's country whatever gl says, and it is minted once and then reused for months.
static const char *VISITOR_BODY =
   "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"}}}";
// search params combine the "Videos" filter (drops the Shorts shelf, channels and playlists) with a sort
// order. index by SortOrder. canonical values from youtube's own filter menu.
static const char *SEARCH_SORT_PARAMS[SORT_COUNT] = {
   "EgIQAQ==",   // SORT_RELEVANCE (youtube's default)
   "CAMSAhAB",   // SORT_VIEWS (popularity / view count, highest first)
};
// The list bodies are string literals, not const char pointers, so -Wall's format check sees every snprintf
// that uses one: a call that forgets the region is a build warning, not a garbage pointer read at run time. The
// check cannot see the ORDER of the arguments (each is a char *), so every call passes gl straight after the
// format, where a grep for the format name followed by gl can count them.
#define SEARCH_BODY_FMT       WEB_CONTEXT_FMT ",\"query\":\"%s\",\"params\":\"%s\"}"      // gl, escaped query, params
// trending feed. YouTube retired the aggregated Trending page in 2025 (FEtrending now redirects to an empty
// home feed); NewPipe/FreeTube/Invidious replaced it with per-category channel-tab browses that still work
// logged-out. see TRENDING_FEEDS for the ids.
#define TRENDING_BODY_FMT     WEB_CONTEXT_FMT ",\"browseId\":\"%s\",\"params\":\"%s\"}"   // gl, browseId, params
#define CONTINUATION_BODY_FMT WEB_CONTEXT_FMT ",\"continuation\":\"%s\"}"                 // gl, token: any feed's next page

static int hex4(const char *p)
{
   int value = 0;
   for (int i = 0; i < 4; i++) {
      char c = p[i], digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else return -1;
      value = value * 16 + digit;
   }
   return value;
}

// copy the json string value of "key" found within [from,end), unescaping the
// usual escapes plus \uXXXX (ascii only - urls and titles are ascii; higher
// code points are dropped). returns 0 if the key was not found, 1 if it was, and 2 if its value did not fit
// `cap` and was cut (readField marks such a text with "...").
static int jsonString(const char *from, const char *end, const char *key, char *out, int cap)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":\"", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) { if (cap) out[0] = 0; return 0; }
   p += patLen;

   int i = 0;
   while (p < end && *p && *p != '"' && i < cap - 1) {
      if (*p != '\\') { out[i++] = *p++; continue; }
      if (++p >= end) break;                       // consume backslash
      char c = *p++;
      switch (c) {
         case 'n': out[i++] = '\n'; break;
         case 't': out[i++] = '\t'; break;
         case 'r': out[i++] = '\r'; break;
         case 'b': out[i++] = '\b'; break;
         case 'f': out[i++] = '\f'; break;
         case 'u': {
            if (p + 4 > end) { p = end; break; }
            int value = hex4(p);
            p += 4;
            if (value >= 0 && value <= 0x7F) out[i++] = (char)value;   // ascii only
            break;
         }
         default: out[i++] = c; break;              // \/ \\ \" and any other: keep literal
      }
   }
   // The cap cut the value short: never end on half a UTF-8 character. A Cyrillic, Greek or Japanese title is
   // 2-3 bytes a character, and a cut one sends the whole label to FONT_POP with a stray letter at its end.
   // Escapes only ever add ASCII, so they cannot leave a lead byte behind.
   int cut = i == cap - 1 && p < end && *p && *p != '"';
   if (cut && ((unsigned char)*p & 0xC0) == 0x80) {
      while (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0x80) i--;   // the cut character's continuation bytes
      if (i > 0 && (unsigned char)out[i - 1] >= 0xC0) i--;               // and its lead byte
   }
   out[i] = 0;
   return cut ? 2 : 1;
}

// A text the viewer reads that its field's size cut short ends in "..." - "The Colin Cowherd Podcast and The Herd
// with Col" read as if the channel were called that. The dots replace the end, at a whole character.
static void markCut(char *out, int cap)
{
   if (cap < 4) return;
   int i = (int)strlen(out);
   if (i > cap - 4) i = cap - 4;                                     // room for "..." and the terminator
   while (i > 0 && ((unsigned char)out[i] & 0xC0) == 0x80) i--;      // never inside a character
   memcpy(out + i, "...", 4);
}

// read the (possibly quoted) integer value of "key" within [from,end). 1 if found.
static int jsonInt(const char *from, const char *end, const char *key, int *out)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return 0;
   p += patLen;
   if (*p == '"') p++;      // some numeric fields are quoted (e.g. lengthSeconds)
   *out = atoi(p);
   return 1;
}

static int matches(const char *input)
{
   if (!input) return 0;
   if (strstr(input, "youtu")) return 1;     // youtube.com / youtu.be url
   return (int)strlen(input) == 11;          // bare 11-char video id
}

static int isIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// copy the 11-char youtube video id from a url (watch?v=, youtu.be/, /shorts/,
// /embed/, /live/) or a bare id into out. returns 1 only if a full 11-char id was found.
static int extractVideoId(const char *input, char *out, int cap)
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id) id = ((int)strlen(input) == 11) ? input : NULL;   // otherwise a bare id
   if (!id) return 0;

   int length = 0;
   while (length < 11 && length < cap - 1 && isIdChar(id[length])) { out[length] = id[length]; length++; }
   out[length] = 0;
   return length == 11;
}

// a bounded strstr: the first needle that starts inside [from, end), or NULL. jsonString/jsonInt run on to the
// end of the response when a key is missing; the audio-track fields below are optional, so each probe stays
// inside its own format object instead of scanning a megabyte for every stream that lacks them.
static const char *findIn(const char *from, const char *end, const char *needle)
{
   int length = (int)strlen(needle);
   for (const char *p = from; p + length <= end; p++)
      if (*p == *needle && memcmp(p, needle, length) == 0) return p;
   return NULL;
}

// case-insensitive substring test; word must be given in lower case
static int containsWord(const char *text, const char *word)
{
   int length = (int)strlen(word);
   for (const char *p = text; *p; p++) {
      int i = 0;
      while (i < length && p[i] && toLowerChar(p[i]) == word[i]) i++;
      if (i == length) return 1;
   }
   return 0;
}

// one key of the url's xtags parameter. An audio stream of a video with several tracks carries them as
// "xtags=acont%3Ddubbed-auto%3Alang%3Dde-DE": acont says what the track is (original, dubbed, dubbed-auto,
// descriptive), lang its language. 1 if found.
static int xtagValue(const char *url, const char *key, char *out, int cap)
{
   out[0] = 0;
   const char *x = strstr(url, "xtags=");
   if (!x) return 0;
   x += 6;
   char encoded[192], tags[192];
   int i = 0;
   while (x[i] && x[i] != '&' && i < (int)sizeof encoded - 1) { encoded[i] = x[i]; i++; }
   encoded[i] = 0;
   if (urlDecode(encoded, tags, sizeof tags) < 0) return 0;

   int keyLength = (int)strlen(key);
   for (const char *p = tags; *p; ) {   // "acont=dubbed-auto:lang=de-DE"
      if (strncmp(p, key, keyLength) == 0 && p[keyLength] == '=') {
         p += keyLength + 1;
         for (i = 0; *p && *p != ':' && i < cap - 1; i++) out[i] = *p++;
         out[i] = 0;
         return 1;
      }
      const char *colon = strchr(p, ':');
      if (!colon) break;
      p = colon + 1;
   }
   return 0;
}

// how good one AAC stream of a track is: itag 140 (AAC-LC, 128k) over the others (139 is 48k HE-AAC), and
// YouTube's normal mix over the dynamic-range-compressed copy it serves beside it
static int audioStreamScore(int itag, int isDrc)
{
   return (itag == 140 ? 2 : 0) + (isDrc ? 0 : 1);
}

// An AAC audio stream that belongs to one of several audio tracks goes into out->audioTracks rather than
// out->formats: one entry per track, holding that track's best stream. The track is named by the reply's
// audioTrack object (id, displayName, audioIsDefault) where it has one, and by the url's xtags otherwise -
// whichever the client returns, the same video can be told apart from its dubs. Returns 1 when the stream
// was such a track stream (taken, or dropped as unplayable), 0 for an ordinary single-track audio stream.
static int collectAudioTrack(const char *object, const char *objectEnd, const StreamFormat *format, StreamInfo *out)
{
   char id[24] = "", name[64] = "", acont[24], lang[16];
   int isDefault = 0;
   const char *block = findIn(object, objectEnd, "\"audioTrack\":{");
   if (block) {
      const char *blockEnd = findIn(block, objectEnd, "}");
      if (!blockEnd) blockEnd = objectEnd;
      if (findIn(block, blockEnd, "\"id\":\""))          jsonString(block, blockEnd, "id", id, sizeof id);
      if (findIn(block, blockEnd, "\"displayName\":\"")) jsonString(block, blockEnd, "displayName", name, sizeof name);
      isDefault = findIn(block, blockEnd, "\"audioIsDefault\":true") != NULL;
   }
   xtagValue(format->url, "acont", acont, sizeof acont);
   xtagValue(format->url, "lang", lang, sizeof lang);
   if (!id[0]) {
      if (!lang[0] && !acont[0]) return 0;   // nothing marks it as one of several: an ordinary audio stream
      snprintf(id, sizeof id, "%.11s.%.11s", lang, acont);
   }
   if (!name[0]) snprintf(name, sizeof name, "%s%s%s", lang[0] ? lang : "Audio", acont[0] ? " " : "", acont);

   // displayName is in the request's language (hl=en): "English (United States) original",
   // "German (auto-dubbed)", "English descriptive". acont says the same without depending on hl.
   int rank;
   if (containsWord(name, "descriptive") || strcmp(acont, "descriptive") == 0)    rank = AUDIO_RANK_DESCRIPTIVE;
   else if (containsWord(name, "original") || strcmp(acont, "original") == 0)     rank = AUDIO_RANK_ORIGINAL;
   else if (containsWord(name, "dubbed") || strncmp(acont, "dubbed", 6) == 0)     rank = AUDIO_RANK_DUBBED;
   else if (isDefault)                                                           rank = AUDIO_RANK_DEFAULT;
   else                                                                          rank = AUDIO_RANK_OTHER;

   if (format->needsCipher || !format->url[0]) return 1;   // one of the tracks' streams, but not playable

   AudioTrack *track = NULL;
   for (int i = 0; i < out->audioTrackCount; i++)
      if (strcmp(out->audioTracks[i].id, id) == 0) { track = &out->audioTracks[i]; break; }
   if (!track) {
      if (out->audioTrackCount >= MAX_AUDIO_TRACKS) return 1;
      track = &out->audioTracks[out->audioTrackCount++];
      memset(track, 0, sizeof *track);
      strCopy(track->id, sizeof track->id, id);
      strCopy(track->name, sizeof track->name, name);
      track->rank = rank;
   }
   if (!track->itag || audioStreamScore(format->itag, format->isDrc) > audioStreamScore(track->itag, track->isDrc)) {
      track->itag = format->itag;
      track->isDrc = format->isDrc;
      strCopy(track->url, sizeof track->url, format->url);
   }
   return 1;
}

// Several audio tracks: sort them best first - stable, so equal ranks keep YouTube's order - and put the best
// one's stream into formats[], where pickBestAudio finds it as the only audio stream on offer.
static void finishAudioTracks(StreamInfo *out)
{
   for (int i = 1; i < out->audioTrackCount; i++) {
      if (out->audioTracks[i - 1].rank >= out->audioTracks[i].rank) continue;   // already in place
      AudioTrack moving = out->audioTracks[i];   // ~2 KB of stack; the resolve workers have 64 KB
      int j = i;
      while (j > 0 && out->audioTracks[j - 1].rank < moving.rank) { out->audioTracks[j] = out->audioTracks[j - 1]; j--; }
      out->audioTracks[j] = moving;
   }
   if (out->audioTrackCount == 0) return;
   if (out->formatCount >= MAX_STREAM_FORMATS) { logError("[yt] no room for the audio stream\n"); return; }

   const AudioTrack *best = &out->audioTracks[0];
   StreamFormat *format = &out->formats[out->formatCount++];
   memset(format, 0, sizeof *format);
   format->itag = best->itag;
   format->hasAudio = 1;
   format->isDrc = best->isDrc;
   strCopy(format->container, sizeof format->container, "mp4");
   strCopy(format->url, sizeof format->url, best->url);
}

// parse one formats array (region [arrayStart, regionEnd)) into out->formats.
// progressive entries are muxed (video+audio); adaptive entries are video-only
// or audio-only, classified from the mimeType.
static void parseFormatArray(const char *arrayStart, const char *regionEnd, int progressive, StreamInfo *out)
{
   const char *scan = arrayStart;
   while (out->formatCount < MAX_STREAM_FORMATS) {
      const char *object = strstr(scan, "\"itag\":");
      if (!object || object >= regionEnd) break;
      const char *next = strstr(object + 7, "\"itag\":");
      const char *objectEnd = (next && next < regionEnd) ? next : regionEnd;

      StreamFormat *format = &out->formats[out->formatCount];
      memset(format, 0, sizeof *format);
      jsonInt(object, objectEnd, "itag", &format->itag);
      jsonInt(object, objectEnd, "width", &format->width);
      jsonInt(object, objectEnd, "height", &format->height);
      jsonInt(object, objectEnd, "fps", &format->fps);
      format->needsCipher = !jsonString(object, objectEnd, "url", format->url, sizeof format->url);

      // mimeType is like: video/mp4; codecs="avc1.640028". only mark a stream playable when its codec is
      // one we can actually decode - H.264 (avc1/avc3) video, AAC (mp4a) audio. newer itags carry AV1/VP9
      // video or Opus audio in an mp4/webm container that our demuxer+decoder can't play (itag 787 = AV1),
      // and picking one of those over a real avc1 stream made the video fail to open.
      char mimeType[64] = "";
      jsonString(object, objectEnd, "mimeType", mimeType, sizeof mimeType);
      strCopy(format->container, sizeof format->container, strstr(mimeType, "webm") ? "webm" : "mp4");
      int isAvc = strstr(mimeType, "avc1") != NULL || strstr(mimeType, "avc3") != NULL;
      int isAac = strstr(mimeType, "mp4a") != NULL;
      // (V3.0.0) VP9 in WebM above 1080p, up to 2560 x 1440: YouTube's 1440p (itag 308 at 60 fps, 271 at 30), which
      // the TEE VP9 building block decodes on the SPEs. Profile 0 only - codecs="vp9" or "vp09.00..." - the HDR
      // ones (vp09.02, 10 bit) it refuses. Lower VP9 stays out: H.264 covers it, and every format kept takes an
      // array slot the AAC audio (listed after all the video) needs.
      int isVp9 = strstr(mimeType, "webm") && (strstr(mimeType, "\"vp9\"") || strstr(mimeType, "vp09.00.")) &&
                  format->height > 1080 && format->height <= 1440 && format->width <= 2560;
      if (progressive)                              format->hasVideo = format->hasAudio = 1;
      // (V3.0.0) Opus in WebM, itag 251 only (the best of YouTube's three): VP9's sound, decoded on the PPU
      else if (strncmp(mimeType, "audio/", 6) == 0) {
         int isOpus = strstr(mimeType, "webm") && strstr(mimeType, "opus") && format->itag == 251;
         format->hasAudio = isAac || isOpus;
         format->isOpus = isOpus && !isAac;
      }
      else                                          { format->hasVideo = isAvc || isVp9; format->isVp9 = isVp9 && !isAvc; }

      // an audio stream of a video with several audio tracks (other languages, AI dubs) is filed by track
      // instead - see collectAudioTrack; the chosen track's stream joins formats[] once the array is read
      int tracked = 0;
      if (format->hasAudio && !format->hasVideo && !format->isOpus) {
         char drc[4];
         format->isDrc = findIn(object, objectEnd, "\"isDrc\":true") != NULL ||
                         (xtagValue(format->url, "drc", drc, sizeof drc) && drc[0] == '1');
         tracked = collectAudioTrack(object, objectEnd, format, out);
      }

      // keep only formats we can decode (H.264 video, VP9 1440p / AAC audio). dropping AV1/Opus frees array slots
      // so the AAC audio - which youtube lists AFTER all the video formats - isn't pushed past the cap.
      if ((format->hasVideo || format->hasAudio) && !tracked) out->formatCount++;
      scan = objectEnd;
   }
}

// parse the caption track list into out->captions. the list ends where the renderer's next field
// starts (entries hold nested arrays, so a plain ']' scan would cut early); each entry leads with
// "baseUrl". the display name sits in the entry's name block as "text" (runs) or "simpleText".
static void parseCaptionTracks(const char *resp, const char *end, StreamInfo *out)
{
   const char *captions = strstr(resp, "\"captionTracks\"");
   if (!captions) return;
   const char *listEnd = strstr(captions, "\"audioTracks\"");
   if (!listEnd) listEnd = strstr(captions, "\"translationLanguages\"");
   if (!listEnd) listEnd = end;

   const char *scan = captions;
   while (out->captionCount < MAX_CAPTION_TRACKS) {
      const char *entry = strstr(scan, "\"baseUrl\"");
      if (!entry || entry >= listEnd) break;
      const char *next = strstr(entry + 1, "\"baseUrl\"");
      const char *entryEnd = (next && next < listEnd) ? next : listEnd;

      CaptionTrack *track = &out->captions[out->captionCount];
      if (jsonString(entry, entryEnd, "baseUrl", track->url, sizeof track->url) &&
          jsonString(entry, entryEnd, "languageCode", track->languageCode, sizeof track->languageCode)) {
         if (!jsonString(entry, entryEnd, "text", track->name, sizeof track->name) &&
             !jsonString(entry, entryEnd, "simpleText", track->name, sizeof track->name))
            strCopy(track->name, sizeof track->name, track->languageCode);
         out->captionCount++;
      }
      scan = entryEnd;
   }
   if (out->captionCount) logInfo("[yt] %d caption track(s)\n", out->captionCount);
}

// parse both stream arrays. iOS returns only adaptiveFormats (separate video and
// audio); WEB/TVHTML5 would also carry a progressive "formats" (muxed) array.
static void parseFormats(const char *resp, const char *end, StreamInfo *out)
{
   const char *streaming = strstr(resp, "\"streamingData\"");
   if (!streaming) return;
   const char *progressive = strstr(streaming, "\"formats\"");
   const char *adaptive    = strstr(streaming, "\"adaptiveFormats\"");

   if (progressive)
      parseFormatArray(progressive, (adaptive && adaptive > progressive) ? adaptive : end, 1, out);
   if (adaptive)
      parseFormatArray(adaptive, end, 0, out);
   finishAudioTracks(out);
}

// The anonymous visitor session. Without it a logged-out player request gets LOGIN_REQUIRED. Minting
// a fresh one every launch is itself a bot signal that re-triggers the wall, so it persists to disk:
// we reuse a saved session and only mint (and save) when there's none, or a saved one goes stale.
//
// The file lives in the directory migrateAppData settled on (see app-paths.h). It used to be built from
// YO_DATA_DIR, which pointed it at /dev_hdd0/yo-player even on a console that refuses that folder: there
// nothing was ever saved, and every launch minted a fresh session - the very thing this cache exists to avoid.
static void visitorCachePath(char *out, int cap)
{
   snprintf(out, cap, "%s/visitor.txt", getAppDataDir());
}

static char visitorData[192];
static int  visitorFromDisk;   // the current session was loaded from the cache (vs minted this launch)

static int loadVisitorData(void)
{
   char path[128];
   visitorCachePath(path, sizeof path);
   return readFile(path, visitorData, sizeof visitorData) > 0 && visitorData[0] != 0;
}

static void saveVisitorData(void)
{
   char path[128];
   visitorCachePath(path, sizeof path);
   writeFile(path, visitorData, strlen(visitorData));
}

// a saved session that hit LOGIN_REQUIRED is stale: forget it so the next play mints a fresh one once.
static void dropVisitorData(void)
{
   char path[128];
   visitorCachePath(path, sizeof path);
   deleteFile(path);
   visitorData[0] = 0;
   visitorFromDisk = 0;
}

static int ensureVisitorData(void)
{
   if (visitorData[0]) return 0;
   if (loadVisitorData()) { visitorFromDisk = 1; return 0; }   // reuse the persisted session

   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
   };
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", VISITOR_URL, headers, 4, VISITOR_BODY, (int)strlen(VISITOR_BODY), resp, RESP_CAP, &respLen, &status);
   int ok = (rc == 0 && status == 200 && jsonString(resp, resp + respLen, "visitorData", visitorData, sizeof visitorData));
   free(resp);

   if (!ok) { logError("[yt] visitor_id failed rc=%d status=%d\n", rc, status); return -1; }
   urlDecode(visitorData, visitorData, sizeof visitorData);   // endpoint returns it percent-encoded (%3D)
   visitorFromDisk = 0;
   logInfo("[yt] visitorData minted (%d chars)\n", (int)strlen(visitorData));
   return 0;
}

// XML text nodes escape '&' as "&amp;"; the BaseURL query string needs it back to fetch.
static void unescapeAmp(char *text)
{
   char *write = text;
   for (char *read = text; *read; ) {
      if (strncmp(read, "&amp;", 5) == 0) { *write++ = '&'; read += 5; }
      else *write++ = *read++;
   }
   *write = 0;
}

// copy the first `attr="..."` (or `<tag>...` with close="<") value found at/after text into out; empty if absent.
static void extractBetween(const char *text, const char *open, const char *close, char *out, int cap)
{
   out[0] = 0;
   const char *start = strstr(text, open);
   if (!start) return;
   start += strlen(open);
   const char *stop = strstr(start, close);
   if (!stop) return;
   int length = (int)(stop - start);
   if (length >= cap) length = cap - 1;
   memcpy(out, start, length);
   out[length] = 0;
}

// a live stream's itag urls are a rolling DVR window whose byte-0 init rolls off, so play by DASH segments
// instead: parse the manifest and, for each adaptiveFormat, replace its url with that Representation's
// segment base ("<base>sq/<n>/...") and record the available sq range (window start .. live edge). kind is
// "dashseg" for a live stream and "dashvod" for one that just ended, whose last segment is its end (video-source.c).
static void parseLiveManifest(const char *resp, const char *end, StreamInfo *out, const char *kind)
{
   char manifestUrl[1400];
   if (!jsonString(resp, end, "dashManifestUrl", manifestUrl, sizeof manifestUrl)) return;

   int cap = 4 * 1024 * 1024;   // live DVR manifests enumerate every segment and can pass 1 MB
   char *mpd = (char *)malloc(cap);
   if (!mpd) return;
   HttpHeader headers[] = { { "User-Agent", PLAYER_UA }, { "Accept-Encoding", "identity" } };
   int length = 0, status = 0;
   if (fetchHttp("GET", manifestUrl, headers, 2, NULL, 0, mpd, cap, &length, &status) != 0 || status != 200) {
      logError("[yt] live manifest fetch status=%d len=%d\n", status, length);
      free(mpd);
      return;
   }
   if (length >= cap - 1) logWarn("[yt] live manifest truncated at %d bytes\n", length);

   for (int i = 0; i < out->formatCount; i++) {
      StreamFormat *format = &out->formats[i];
      char idTag[24];
      snprintf(idTag, sizeof idTag, "id=\"%d\"", format->itag);
      const char *rep = strstr(mpd, idTag);
      if (!rep) continue;
      const char *repEnd = strstr(rep, "</Representation>");
      if (!repEnd) repEnd = mpd + length;

      char base[MAX_STREAM_URL];
      extractBetween(rep, "<BaseURL>", "</BaseURL>", base, sizeof base);
      if (!base[0] || strstr(rep, "<BaseURL>") > repEnd) continue;   // this rep has no own BaseURL
      unescapeAmp(base);

      // first + last media="sq/<n>/..." within this representation = the window start and the live edge
      long startSq = -1, edgeSq = -1;
      for (const char *media = rep; (media = strstr(media, "media=\"sq/")) != NULL && media < repEnd; media++) {
         long number = 0;
         for (const char *digit = media + 10; *digit >= '0' && *digit <= '9'; digit++) number = number * 10 + (*digit - '0');
         if (startSq < 0) startSq = number;
         edgeSq = number;
      }
      if (edgeSq < 0) continue;

      format->isLiveSegmented = 1;
      format->liveStartSq = startSq;
      format->liveEdgeSq = edgeSq;
      // the player opens this through the live segment source (see video-source / live-source)
      snprintf(format->url, sizeof format->url, "%s|%ld|%ld|%s", kind, startSq, edgeSq, base);
      logInfo("[yt] live itag %d: sq %ld..%ld baseTail=%s\n", format->itag, startSq, edgeSq,
              (int)strlen(base) > 40 ? base + strlen(base) - 40 : base);
   }
   free(mpd);
}

// The token-exempt clients (VISIONOS, and ANDROID_VR before it) answer live videos WITHOUT a
// dashManifestUrl, so parseLiveManifest has nothing to read and the player would open a rolling
// live url as if it were a static file - a few seconds of video, then "couldn't open stream".
//
// The manifest is not actually needed: googlevideo serves those same urls segment-wise via
// "&sq=<n>" and reports the current live edge in the X-Head-Seqnum response header. Probe the
// first format for that header and build the same dashseg descriptors parseLiveManifest would
// have produced, in the query form ("<url>&sq=") that live-source appends the number to. A stream that just
// ended answers the same way, with its last segment as the head.
static void parseLiveFromSeqnum(StreamInfo *out, const char *kind)
{
   if (out->formatCount <= 0 || !out->formats[0].url[0]) return;

   char seqnum[40] = "";
   // Only the head is wanted: a one-byte buffer reads none of the body. On a stream that had just ended the
   // server sent the head at once and then held the 1-byte body until the connection broke, ~7.6 s later
   // (TEE's console, 2026-11-07, Test11) - and the failed read made the probe throw away the head it had.
   char body[1];
   HttpHeader headers[] = { { "User-Agent", PLAYER_UA }, { "Range", "bytes=0-0" } };
   // findHeader lowercases only the response side, so the name has to be lowercase here
   HttpHeaderCapture capture = { "x-head-seqnum", seqnum, sizeof seqnum };
   int length = 0, status = 0;
   int rc = fetchHttpCapturing("GET", out->formats[0].url, headers, 2, NULL, 0,
                               body, sizeof body, &length, &status, &capture);
   if (rc != 0 && !seqnum[0]) {   // the head is taken before the body is read: it counts whenever it came
      logError("[yt] live seqnum probe failed (status=%d)\n", status);
      return;
   }

   long edgeSq = 0;
   for (const char *digit = seqnum; *digit >= '0' && *digit <= '9'; digit++)
      edgeSq = edgeSq * 10 + (*digit - '0');
   if (edgeSq <= 0) {
      logError("[yt] live probe: no X-Head-Seqnum (status=%d)\n", status);
      return;
   }

   int rewritten = 0;
   for (int i = 0; i < out->formatCount; i++) {
      StreamFormat *format = &out->formats[i];
      if (!format->url[0]) continue;

      char rebuilt[MAX_STREAM_URL];
      // "&sq=" is 4 chars; snprintf truncates rather than overflows, so drop anything that
      // would not survive the round trip instead of handing the player a cut-off url.
      if (snprintf(rebuilt, sizeof rebuilt, "%s|0|%ld|%s&sq=", kind, edgeSq, format->url)
          >= (int)sizeof rebuilt) continue;

      strCopy(format->url, sizeof format->url, rebuilt);
      format->isLiveSegmented = 1;
      format->liveStartSq = 0;
      format->liveEdgeSq = edgeSq;
      rewritten++;
   }
   logInfo("[yt] live via X-Head-Seqnum: edge sq %ld, %d/%d formats\n",
           edgeSq, rewritten, out->formatCount);
}

static int extract(const char *input, StreamInfo *out)
{
   memset(out, 0, sizeof *out);

   char videoId[16];
   if (!extractVideoId(input, videoId, sizeof videoId)) return -1;
   if (ensureVisitorData() != 0) return -1;

   char body[1024];
   int bodyLen = snprintf(body, sizeof body, PLAYER_BODY_FMT, visitorData, videoId);

   HttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", PLAYER_UA },
      { "X-YouTube-Client-Name", "101" },
      { "X-YouTube-Client-Version", PLAYER_VERSION },
      { "X-Goog-Visitor-Id", visitorData },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", PLAYER_URL, headers, 7, body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0)        { logError("[yt] player POST failed rc=%d\n", rc); free(resp); return rc; }
   if (status != 200) { logError("[yt] player POST status=%d respLen=%d\n", status, respLen); free(resp); return -status; }
   const char *end = resp + respLen;

   // metadata
   const char *details = strstr(resp, "\"videoDetails\"");
   jsonString(details ? details : resp, end, "title", out->title, sizeof out->title);
   jsonString(details ? details : resp, end, "shortDescription", out->description, sizeof out->description);
   char lenText[16];
   if (jsonString(details ? details : resp, end, "lengthSeconds", lenText, sizeof lenText))
      out->durationSeconds = atoi(lenText);

   parseCaptionTracks(resp, end, out);
   parseFormats(resp, end, out);

   // Diagnostic: the whole ladder this client was offered. stream-select only ever reports the
   // one format it picked, which cannot tell you WHY - e.g. whether a 1080p30 rendition existed
   // at all, or whether the 1080p60 was simply the only 1080p on offer.
   for (int i = 0; i < out->formatCount; i++) {
      const StreamFormat *f = &out->formats[i];
      logInfo("[yt] fmt itag=%d %dx%d %dfps %s vid=%d aud=%d cipher=%d url=%d drc=%d\n",
              f->itag, f->width, f->height, f->fps, f->container,
              f->hasVideo, f->hasAudio, f->needsCipher, f->url[0] ? 1 : 0, f->isDrc);
   }
   // and every audio track, best first: the first one is what plays
   for (int i = 0; i < out->audioTrackCount; i++) {
      const AudioTrack *t = &out->audioTracks[i];
      logInfo("[yt] audio track %d: %s [%s] rank=%d itag=%d%s\n", i, t->name, t->id, t->rank, t->itag, t->isDrc ? " drc" : "");
   }

   // a currently-live stream (isLive:true) is a rolling DVR window, so the init/moov at byte 0 may have
   // rolled off - it needs the DASH manifest's explicit init + segment template rather than a static open.
   {
      int liveNow      = strstr(resp, "\"isLive\":true") != NULL;
      // A stream that just ended: for a while YouTube serves its recording only as the live segments, sq 0 to the
      // last one (204 past it), until it has made a video of it - opened as a file it has no moov ("couldn't open
      // stream" on TEE's console, 2026-11-07). It plays like a live stream whose edge stays put and ends there.
      int postLive     = !liveNow && strstr(resp, "\"isPostLiveDvr\":true") != NULL;
      const char *dash = strstr(resp, "\"dashManifestUrl\"");
      out->isLive = liveNow;
      out->isPostLive = postLive;
      if (liveNow || postLive) {
         logInfo("[yt] %s (dash=%d formats=%d dur=%d)\n", liveNow ? "live stream" : "live stream that just ended",
                 dash ? 1 : 0, out->formatCount, out->durationSeconds);
         // the rewind: the streamer's DVR switch, and every adaptive format's segment length and window (the
         // same for all of a stream's formats). Seen 2026-11-07: DVR on for 2 of 6 live streams, 5 s segments
         // (2 s for low latency), a 12 h window; googlevideo still served segments 14 h back with DVR off.
         out->liveDvr = strstr(resp, "\"isLiveDvrEnabled\":true") != NULL;
         const char *streaming = strstr(resp, "\"streamingData\"");
         jsonInt(streaming, end, "targetDurationSec", &out->liveSegmentSec);
         jsonInt(streaming, end, "maxDvrDurationSec", &out->liveWindowSec);
         logInfo("[yt] live rewind %s, %d s segments, window %d s\n", out->liveDvr ? "on" : "off",
                 out->liveSegmentSec, out->liveWindowSec);
         const char *kind = postLive ? "dashvod" : "dashseg";
         parseLiveManifest(resp, end, out, kind);
         int segmented = 0;
         for (int i = 0; i < out->formatCount; i++)
            if (out->formats[i].isLiveSegmented) { segmented = 1; break; }
         // no dashManifestUrl in the reply (every token-exempt client): fall back to the edge probe
         if (!segmented) parseLiveFromSeqnum(out, kind);
      }
   }

   if (out->formatCount == 0) {
      char statusText[32] = "";
      const char *play = strstr(resp, "\"playabilityStatus\"");
      jsonString(play ? play : resp, end, "status", statusText, sizeof statusText);
      logError("[yt] no formats, respLen=%d playability=%s\n", respLen, statusText);
      if (visitorFromDisk && strcmp(statusText, "LOGIN_REQUIRED") == 0) dropVisitorData();   // stale session
   } else {
      logInfo("[yt] resolved %d formats\n", out->formatCount);
      if (!visitorFromDisk) { saveVisitorData(); visitorFromDisk = 1; }   // persist a session that works
   }

   free(resp);
   return 0;
}

// copy src into out as a json string body value, escaping " and \ (and dropping
// control chars). the query comes from the on-screen keyboard.
static void jsonEscape(const char *src, char *out, int cap)
{
   int i = 0;
   for (; *src && i < cap - 2; src++) {
      unsigned char c = (unsigned char)*src;
      if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
      else if (c >= 0x20)        { out[i++] = (char)c; }
   }
   out[i] = 0;
}

// pull a nested string from a videoRenderer sub-object: find `container` (e.g. "\"ownerText\""), then read
// `key` (e.g. "text" for a runs array, "simpleText" for a plain value) within the block. Missing = left empty.
static void readField(const char *block, const char *end, const char *container, const char *key, char *out, int cap)
{
   const char *pos = strstr(block, container);
   // every field read here is text on screen (title, channel, duration, date): a cut one says so
   if (pos && pos < end && jsonString(pos, end, key, out, cap) == 2) markCut(out, cap);
}

// the next feed item at or after `from`. search uses "videoRenderer", trending shelves "gridVideoRenderer",
// and channel pages the newer "lockupViewModel". returns the nearest of the three (NULL if none).
static const char *nextItemBlock(const char *from)
{
   static const char *keys[] = { "\"videoRenderer\"", "\"gridVideoRenderer\"", "\"lockupViewModel\"" };
   const char *best = NULL;
   for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      const char *hit = strstr(from, keys[i]);
      if (hit && (!best || hit < best)) best = hit;
   }
   return best;
}

static int isDuplicate(const SearchResults *out, const char *videoId)
{
   for (int i = 0; i < out->count; i++) if (strcmp(out->items[i].videoId, videoId) == 0) return 1;
   return 0;
}

// copy the first "content":"..." value within [from,end) whose text ends with `suffix` (e.g. " views",
// " ago"). lockupViewModel metadata rows are plain content strings, not keyed by field.
static void readContentEndingWith(const char *from, const char *end, const char *suffix, char *out, int cap)
{
   int suffixLen = (int)strlen(suffix);
   const char *p = from;
   while ((p = strstr(p, "\"content\":\"")) != NULL && p + 11 <= end) {
      p += 11;                                       // past "content":"
      const char *close = memchr(p, '"', end - p);
      if (!close) break;
      int length = (int)(close - p);
      if (length >= suffixLen && length < cap && strncmp(close - suffixLen, suffix, suffixLen) == 0) {
         memcpy(out, p, length); out[length] = 0; return;
      }
      p = close + 1;
   }
}

// the owning channel's UC id: the first "browseId":"UC..." in the item block (the video owner for a
// videoRenderer, the author for a lockup). left empty if absent.
static void readChannelId(const char *block, const char *end, char *out, int cap)
{
   const char *p = strstr(block, "\"browseId\":\"UC");
   if (p && p < end) jsonString(p, end, "browseId", out, cap);
   else if (cap) out[0] = 0;
}

// One past the closing brace of the JSON object whose '{' is at `open`, or `end` when it does not close before
// it. Strings are skipped whole, escapes included, so a brace inside a title cannot end the object early.
static const char *objectEnd(const char *open, const char *end)
{
   int depth = 0, inString = 0;
   for (const char *p = open; p < end; p++) {
      char c = *p;
      if (inString) {
         if (c == '\\') p++;             // the escaped character, whatever it is
         else if (c == '"') inString = 0;
      }
      else if (c == '"') inString = 1;
      else if (c == '{') depth++;
      else if (c == '}' && --depth == 0) return p + 1;
   }
   return end;
}

// The view count, read only inside the item's own "viewCountText" object. A video gives it as one simpleText
// ("6,100 views"); a live stream gives it as runs ("52,106" + " watching"), which used to leave the field empty
// - or the scan ran on past the field and took a later simpleText such as the overlay's "PREMIERE".
static void readViewCount(const char *block, const char *end, char *out, int cap)
{
   out[0] = 0;
   const char *field = findIn(block, end, "\"viewCountText\":{");   // a null count has no object at all
   if (!field) return;
   const char *fieldEnd = objectEnd(field + 16, end);              // 16 = strlen("\"viewCountText\":")
   if (findIn(field, fieldEnd, "\"simpleText\":\"")) { jsonString(field, fieldEnd, "simpleText", out, cap); return; }

   int length = 0;
   for (const char *run = field; (run = findIn(run, fieldEnd, "\"text\":\"")) != NULL; run += 8) {
      char part[24];
      jsonString(run, fieldEnd, "text", part, sizeof part);
      int partLength = (int)strlen(part), room = cap - 1 - length, cut = partLength > room;
      if (cut) {   // keep what fits, and never half a character
         partLength = room;
         while (partLength > 0 && ((unsigned char)part[partLength] & 0xC0) == 0x80) partLength--;
      }
      memcpy(out + length, part, partLength);
      length += partLength;
      out[length] = 0;
      if (cut) break;
   }
}

// fill one result from a videoRenderer (search) or gridVideoRenderer (trending) block. 1 if it has a title.
static int parseRendererItem(const char *block, const char *end, SearchResult *item)
{
   if (!jsonString(block, end, "videoId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;
   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"title\"", "text", item->title, sizeof item->title);
   if (!item->title[0])   // gridVideoRenderer (trending) uses a plain simpleText title
      readField(block, end, "\"title\"", "simpleText", item->title, sizeof item->title);
   readField(block, end, "\"lengthText\"",        "simpleText", item->duration,  sizeof item->duration);
   // A trending item (gridVideoRenderer) has no lengthText: its duration is only the thumbnail's time overlay, so
   // the Trending cards showed none. It is taken from there when the overlay is a plain duration ("style":
   // "DEFAULT"), never from a LIVE or UPCOMING one, whose text is a word rather than a time.
   if (!item->duration[0]) {
      static const char OVERLAY[] = "\"thumbnailOverlayTimeStatusRenderer\":{";
      const char *overlay = findIn(block, end, OVERLAY);
      if (overlay) {
         const char *open  = overlay + sizeof OVERLAY - 2;   // its '{'
         const char *close = objectEnd(open, end);
         if (findIn(open, close, "\"style\":\"DEFAULT\""))
            jsonString(open, close, "simpleText", item->duration, sizeof item->duration);
      }
   }
   readField(block, end, "\"ownerText\"",         "text",       item->author,    sizeof item->author);
   if (!item->author[0])   // trending items carry the channel in shortBylineText instead
      readField(block, end, "\"shortBylineText\"", "text", item->author, sizeof item->author);
   readViewCount(block, end, item->views, sizeof item->views);
   readField(block, end, "\"publishedTimeText\"", "simpleText", item->published, sizeof item->published);
   // browse surfaces mark a live stream with the time-status overlay, search and the Gaming live tab only with
   // a badge. Bounded to the item: an unbounded search ran on through the rest of the reply for every video.
   item->isLive = findIn(block, end, "\"style\":\"LIVE\"") != NULL ||
                  findIn(block, end, "\"style\":\"BADGE_STYLE_TYPE_LIVE_NOW\"") != NULL;
   return item->title[0] != 0;
}

// fill one result from a lockupViewModel VIDEO block (youtube's newer feed-item format, seen on channel
// pages and other surfaces). 1 if it's a playable video with a title; albums/playlists (other contentTypes) are skipped.
static int parseLockupItem(const char *block, const char *end, SearchResult *item)
{
   const char *videoType = strstr(block, "\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\"");
   if (!videoType || videoType >= end) return 0;
   if (!jsonString(block, end, "contentId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;

   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"lockupMetadataViewModel\"", "content", item->title, sizeof item->title);
   readField(block, end, "\"metadataParts\"",           "content", item->author, sizeof item->author);
   // on a channel's own page the first metadata part is the view count, not the channel name - drop it so
   // the caller can substitute the real channel name (the channel is implied on that page).
   if (strstr(item->author, " views") || strstr(item->author, " watching")) item->author[0] = 0;
   readField(block, end, "\"thumbnailBadgeViewModel\"", "text",    item->duration, sizeof item->duration);
   readContentEndingWith(block, end, " views", item->views, sizeof item->views);
   readContentEndingWith(block, end, " ago",   item->published, sizeof item->published);
   item->isLive = 0;
   return item->title[0] != 0;
}

// scan a WEB innertube response for feed items (search, trending shelves and channel pages use different
// renderers, dispatched per block) and fill out->items. dedupes by videoId - youtube repeats a
// video across shelves.
static void parseVideoResults(const char *resp, int respLen, SearchResults *out)
{
   const char *end = resp + respLen;
   const char *scan = resp;
   while (out->count < MAX_SEARCH_RESULTS) {
      const char *block = nextItemBlock(scan);
      if (!block) break;
      const char *next = nextItemBlock(block + 1);
      const char *blockEnd = (next && next < end) ? next : end;

      SearchResult *item = &out->items[out->count];
      memset(item, 0, sizeof *item);
      int ok = (strncmp(block + 1, "lockupViewModel", 15) == 0)
             ? parseLockupItem(block, blockEnd, item)
             : parseRendererItem(block, blockEnd, item);
      // drop members-only videos: they resolve to UNPLAYABLE for an anonymous session (no stream, no author)
      const char *membersBadge = strstr(block, "Members only");
      if (membersBadge && membersBadge < blockEnd) ok = 0;
      if (ok && !isDuplicate(out, item->videoId)) out->count++;
      scan = blockEnd;
   }

   // the "load more" token for the next page. anchor on the OBJECT ("...":{) not the bare name - youtube's
   // response carries a schema list of all renderer names ("...","...") and the bare string matches that
   // first, yielding a garbage token. the real continuationItemRenderer holds continuationCommand > token.
   const char *more = strstr(resp, "\"continuationItemRenderer\":{");
   if (more && more < end) jsonString(more, end, "token", out->continuation, sizeof out->continuation);

   // channel feeds carry the channel's own title in metadata; used to label videos on a channel page that
   // omit their author (the channel is implied). absent in search/trending responses (left empty). anchor on
   // the OBJECT ("...":{) - the bare name appears first in youtube's schema list, and the title after it is a
   // tab ("Home"), not the channel name.
   const char *channelMeta = strstr(resp, "\"channelMetadataRenderer\":{");
   if (channelMeta && channelMeta < end) jsonString(channelMeta, end, "title", out->channelName, sizeof out->channelName);

   // channel Videos feeds sort by Latest, Popular or Oldest, each with a continuation token the screen re-browses
   // with. YouTube used to send them as three chips (chipViewModel with the text, the token after it); it now sends
   // ONE drop-down chip, "Latest", whose sheet lists the three as listItemViewModel entries, each with its token -
   // so only Latest was found, and Popular and Oldest loaded the plain first page: the Latest list again (seen in
   // RPCS3, Nov 2026; the sheet's tokens answer with their own order, no session needed). The sheet is read first,
   // from the drop-down chip on, and the old chips stay as the fallback. Absent in search/trending (left empty).
   static const char *SORT_CHIP_LABELS[3] = { "Latest", "Popular", "Oldest" };
   const char *sheet = strstr(resp, "\"CHIP_VIEW_MODEL_DISPLAY_TYPE_DROP_DOWN\"");
   for (int chip = 0; chip < 3; chip++) {
      char pat[64];
      const char *found = NULL;
      if (sheet && sheet < end) {
         snprintf(pat, sizeof pat, "\"listItemViewModel\":{\"title\":{\"content\":\"%s\"", SORT_CHIP_LABELS[chip]);
         found = strstr(sheet, pat);
      }
      if (!found || found >= end) {
         snprintf(pat, sizeof pat, "\"chipViewModel\":{\"text\":\"%s\"", SORT_CHIP_LABELS[chip]);
         found = strstr(resp, pat);
      }
      if (found && found < end) jsonString(found, end, "token", out->sortTokens[chip], MAX_SORT_TOKEN);
   }
}

// YouTube names the country it built a page 1 for in its top bar ("countryCode":"DE", near the end of the reply)
// and names none for the United States. A code it does not take gets HTTP 200, an empty page and no country,
// which in the grid looks exactly like an empty list - so the log says which region YouTube really used.
// Continuation replies have no top bar and are not checked. One line whenever the answer changes, not one per
// page. The list workers run one at a time; were two ever to overlap, a log line would repeat or go missing.
static void logRegionEcho(const char *resp, const char *end, const char *sent, int itemCount)
{
   static char loggedSent[4], loggedUsed[8];
   static int  loggedSuspect = -1;

   char named[8] = "";
   const char *badge = strstr(resp, "\"countryCode\":\"");
   if (badge && badge < end) jsonString(badge, end, "countryCode", named, sizeof named);
   int isUs = strcmp(sent, "US") == 0;
   const char *used = named[0] ? named : isUs ? "US" : "none";   // no badge is the normal answer for the US
   int mismatch = named[0] && strcmp(named, sent) != 0;
   int suspect = !named[0] && !isUs && itemCount == 0;

   if (strcmp(loggedSent, sent) == 0 && strcmp(loggedUsed, used) == 0 && loggedSuspect == suspect) return;
   strCopy(loggedSent, sizeof loggedSent, sent);
   strCopy(loggedUsed, sizeof loggedUsed, used);
   loggedSuspect = suspect;
   if (mismatch)     logWarn("[yt] region: sent %s, YouTube used %s\n", sent, used);
   else if (suspect) logWarn("[yt] region: sent %s, YouTube used none and sent no items - it may not take this code\n", sent);
   else              logInfo("[yt] region: sent %s, YouTube used %s\n", sent, used);
}

// a character of a session as YouTube hands it out: base64, url-safe or not
static int isSessionChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '+' || c == '/' || c == '=';
}

// A whole session, and nothing else. It goes out again as a header line, so a malformed reply must never be able
// to smuggle a CR or LF into the next request.
static int isSessionToken(const char *text)
{
   int length = 0;
   while (isSessionChar(text[length])) length++;
   return length > 0 && text[length] == 0;
}

// The reply's own session: its first "visitorData", which is responseContext's (it leads every reply), percent-
// decoded as ensureVisitorData does. "" when there is none, or nothing that passes for one whole.
static void captureSession(const char *resp, const char *end, char *out, int cap)
{
   char raw[128];
   out[0] = 0;
   if (!jsonString(resp, end, "visitorData", raw, sizeof raw) || (int)strlen(raw) >= (int)sizeof raw - 1) return;
   // as it comes, percent-encoded ("%3D"): nothing but session characters and '%', so that urlDecode - which stops
   // at a blank, '?' or CR - reads it whole instead of leaving a cut-off start that would pass the check below
   for (const char *c = raw; *c; c++) if (!isSessionChar(*c) && *c != '%') return;
   if (urlDecode(raw, out, cap) < 0 || (int)strlen(out) >= cap - 1 || !isSessionToken(out)) out[0] = 0;
}

// What a WEB list request needs beyond its body. Search, Trending and channel pages use none of it (postAndParse);
// the Live page uses the session parts and the append.
typedef struct {
   const char *pageOneGl;      // the region a page-1 body was built with, checked against the reply (logRegionEcho);
                               // NULL for a continuation, whose reply names no country
   const char *visitorId;      // sent as X-Goog-Visitor-Id when set: a session that passed isSessionToken
   char       *replyVisitor;   // receives the reply's own session (captureSession); NULL = not wanted
   int         replyVisitorCap;
   int         append;         // add the reply's items to out's (deduped by videoId) instead of starting afresh. out
                               // is then left alone unless an answer comes, whose next-page token replaces out's
} WebRequest;

// POST a WEB innertube endpoint with a prebuilt json body, then parse videoRenderers from the response.
static int postAndParseWith(const char *url, const char *body, int bodyLen, const WebRequest *request, SearchResults *out)
{
   if (!request->append) memset(out, 0, sizeof *out);
   if (request->replyVisitor) request->replyVisitor[0] = 0;
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   HttpHeader headers[7] = {   // WEB_UA is a pointer, so these can't be a file-scope const array
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int headerCount = 6;
   if (request->visitorId && request->visitorId[0]) {   // last, as it was sent when the echo was probed
      headers[6].name = "X-Goog-Visitor-Id";
      headers[6].value = request->visitorId;
      headerCount = 7;
   }

   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", url, headers, headerCount, body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0)        { free(resp); return rc; }
   if (status != 200) { free(resp); return -status; }

   int before = out->count;
   if (request->append) out->continuation[0] = 0;   // the next page is the one this reply names, or none
   parseVideoResults(resp, respLen, out);
   if (request->pageOneGl) logRegionEcho(resp, resp + respLen, request->pageOneGl, out->count - before);
   if (request->replyVisitor) captureSession(resp, resp + respLen, request->replyVisitor, request->replyVisitorCap);
   free(resp);
   return 0;
}

// pageOneGl as in WebRequest.
static int postAndParse(const char *url, const char *body, int bodyLen, const char *pageOneGl, SearchResults *out)
{
   WebRequest request = { pageOneGl, NULL, NULL, 0, 0 };
   return postAndParseWith(url, body, bodyLen, &request, out);
}

// snprintf answers with the length it WOULD have written, and the body is sent bodyLen bytes long: one that did
// not fit is refused rather than sent cut short (or read past its buffer).
static int refuseBody(int bodyLen, SearchResults *out)
{
   logError("[yt] request body of %d bytes does not fit its buffer, not sent\n", bodyLen);
   memset(out, 0, sizeof *out);
   return -1;
}

static int search(const char *query, SortOrder sort, const char *continuation, SearchResults *out)
{
   const char *gl = getContentRegionCode();   // read once: the whole request uses this one value
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation);
   else {
      char escaped[256];
      jsonEscape(query, escaped, sizeof escaped);
      if (sort < 0 || sort >= SORT_COUNT) sort = SORT_RELEVANCE;
      bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, escaped, SEARCH_SORT_PARAMS[sort]);
   }
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   return postAndParse(SEARCH_URL, body, bodyLen, continuation ? NULL : gl, out);
}

// the surviving per-category channel-tab feeds, each a real feed logged-out. order matches TrendingCategory.
static const struct { const char *browseId, *params; } TRENDING_FEEDS[TREND_COUNT] = {
   { "UCOpNcN46UbXVtpKMrmU4Abg", "Egh0cmVuZGluZw%3D%3D" },              // TREND_GAMING
   { "UCEgdi0XIXXZ-qJOFPf4JSKw", "EglzcG9ydHN0YWK4AQCSAwDyBgQKAjIA" },  // TREND_SPORTS
   { "FEpodcasts_destination",   "qgcCCAM%3D" },                        // TREND_PODCASTS
};

static int fetchTrending(TrendingCategory category, const char *continuation, SearchResults *out)
{
   if (category < 0 || category >= TREND_COUNT) return -1;
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, continuation);
   else bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, TRENDING_FEEDS[category].browseId, TRENDING_FEEDS[category].params);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   return postAndParse(BROWSE_URL, body, bodyLen, continuation ? NULL : gl, out);
}

// a channel's Videos tab (sorted Latest by default). token NULL = first page via the videos-tab params;
// else a sort-chip or next-page continuation token. the response fills out->sortTokens for the sort chips.
#define CHANNEL_VIDEOS_PARAMS "EgZ2aWRlb3PyBgQKAjoA"
static int getChannel(const char *channelId, const char *token, SearchResults *out)
{
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (token) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, token);
   else       bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, channelId, CHANNEL_VIDEOS_PARAMS);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   return postAndParse(BROWSE_URL, body, bodyLen, token ? NULL : gl, out);
}

// ---- the signed-in account's lists (account.h) ----
//
// YouTube answers a request carrying the sign-in only for its TV client (probed in Nov 2026: the WEB client gets
// HTTP 400 with the same token), so these go out as the TV app and come back in its format. Whatever the page
// around them - shelves on the home feed, a grid in the history, a playlist's list - every item is a "tileRenderer",
// a video, a Short, a channel or a playlist by its contentType; the home feed's ad is one with none. Only videos are
// kept, as everywhere in the app. Playback stays anonymous (extract): nothing that plays depends on the sign-in.

#define TV_CONTEXT_FMT \
   "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"" ACCOUNT_TV_CLIENT_VERSION "\"," \
   "\"hl\":\"en\",\"gl\":\"%s\"}}"
#define TV_BROWSE_BODY_FMT       TV_CONTEXT_FMT ",\"browseId\":\"%s\"}"       // gl, browseId
#define TV_CONTINUATION_BODY_FMT TV_CONTEXT_FMT ",\"continuation\":\"%s\"}"   // gl, token
#define AUTHORIZATION_CAP        3072                                         // "Bearer " and an access token

// in AccountFeed order (extractor.h)
static const char *ACCOUNT_BROWSE_IDS[ACCOUNT_FEED_COUNT] = {
   "FEwhat_to_watch",   // ACCOUNT_FEED_HOME
   "FEsubscriptions",   // ACCOUNT_FEED_SUBSCRIPTIONS
   "FEhistory",         // ACCOUNT_FEED_HISTORY
   "VLWL",              // ACCOUNT_FEED_WATCH_LATER
   "FEchannels",        // ACCOUNT_FEED_CHANNELS
};

// text into a field of its own size; one that does not fit ends in "..." at a whole character
static void copyText(char *out, int cap, const char *text)
{
   int length = (int)strlen(text);
   if (length < cap) { memcpy(out, text, length + 1); return; }
   memcpy(out, text, cap - 1);
   out[cap - 1] = 0;
   markCut(out, cap);
}

static int endsWith(const char *text, const char *suffix)
{
   int length = (int)strlen(text), suffixLength = (int)strlen(suffix);
   return length >= suffixLength && strcmp(text + length - suffixLength, suffix) == 0;
}

// A TV text object's words: its "simpleText", or its "runs" joined. Every probe stays inside [object, end): an
// unbounded search would run on through the rest of a reply that can pass 700 KB, once per field of every tile.
static void readTextObject(const char *object, const char *end, char *out, int cap)
{
   out[0] = 0;
   if (findIn(object, end, "\"simpleText\":\"")) {
      if (jsonString(object, end, "simpleText", out, cap) == 2) markCut(out, cap);
      return;
   }
   int length = 0;
   for (const char *run = object; (run = findIn(run, end, "\"text\":\"")) != NULL; run += 8) {
      char part[160];
      int cut = jsonString(run, end, "text", part, sizeof part) == 2;
      int partLength = (int)strlen(part), room = cap - 1 - length;
      if (partLength > room) {   // keep what fits, and never half a character
         partLength = room;
         cut = 1;
         while (partLength > 0 && ((unsigned char)part[partLength] & 0xC0) == 0x80) partLength--;
      }
      memcpy(out + length, part, partLength);
      length += partLength;
      out[length] = 0;
      if (cut) { markCut(out, cap); break; }
   }
}

// One video tile. Its lines under the title are the channel, then the counts: a badge or two ("4K"), "199K views",
// "•", "3 weeks ago" - just "23 views" in the history, "1.2K watching" for a stream on air. 1 when it has a title.
static int parseTileItem(const char *tile, const char *end, SearchResult *item)
{
   if (!jsonString(tile, end, "contentId", item->videoId, sizeof item->videoId) || strlen(item->videoId) != 11) return 0;
   const char *channel = findIn(tile, end, "\"browseId\":\"UC");   // the long-press menu's "Go to channel"; none on
   if (channel) jsonString(channel, end, "browseId", item->channelId, sizeof item->channelId);   // home tiles

   static const char META[] = "\"tileMetadataRenderer\":{";
   const char *meta = findIn(tile, end, META);
   if (!meta) return 0;
   const char *metaEnd = objectEnd(meta + sizeof META - 2, end);
   static const char TITLE[] = "\"title\":{";
   const char *title = findIn(meta, metaEnd, TITLE);
   if (title) readTextObject(title, objectEnd(title + sizeof TITLE - 2, metaEnd), item->title, sizeof item->title);

   static const char LINE[] = "\"lineRenderer\":{", TEXT[] = "\"text\":{";
   int line = 0;
   for (const char *at = meta; (at = findIn(at, metaEnd, LINE)) != NULL; line++) {
      const char *lineEnd = objectEnd(at + sizeof LINE - 2, metaEnd);
      for (const char *text = at; (text = findIn(text, lineEnd, TEXT)) != NULL; ) {
         const char *textEnd = objectEnd(text + sizeof TEXT - 2, lineEnd);
         char words[96];
         readTextObject(text, textEnd, words, sizeof words);
         if (line == 0) { if (!item->author[0]) copyText(item->author, sizeof item->author, words); }
         else if (endsWith(words, " views") || endsWith(words, " view")) copyText(item->views, sizeof item->views, words);
         else if (endsWith(words, " watching")) { copyText(item->views, sizeof item->views, words); item->isLive = 1; }
         else if (endsWith(words, " ago") || strncmp(words, "Streamed ", 9) == 0)
            copyText(item->published, sizeof item->published, words);
         text = textEnd;
      }
      at = lineEnd;
   }

   // the time on the thumbnail: a plain duration ("style":"DEFAULT"), or LIVE - never UPCOMING's word
   static const char OVERLAY[] = "\"thumbnailOverlayTimeStatusRenderer\":{";
   const char *overlay = findIn(tile, end, OVERLAY);
   if (overlay) {
      const char *open = overlay + sizeof OVERLAY - 2, *close = objectEnd(open, end);
      if (findIn(open, close, "\"style\":\"DEFAULT\"") && findIn(open, close, "\"simpleText\":\""))
         jsonString(open, close, "simpleText", item->duration, sizeof item->duration);
      else if (findIn(open, close, "\"style\":\"LIVE\"")) item->isLive = 1;
   }
   if (item->isLive) item->duration[0] = 0;
   return item->title[0] != 0;
}

// A channel tile (the account's channel list): its id and name, its handle ("@217ae") in author and its subscriber
// count ("4.6M subscribers") in views - the tile's two lines.
static int parseChannelTile(const char *tile, const char *end, SearchResult *item)
{
   if (!jsonString(tile, end, "contentId", item->channelId, sizeof item->channelId) ||
       strncmp(item->channelId, "UC", 2) != 0) return 0;
   static const char META[] = "\"tileMetadataRenderer\":{", TITLE[] = "\"title\":{";
   static const char LINE[] = "\"lineRenderer\":{", TEXT[] = "\"text\":{";
   const char *meta = findIn(tile, end, META);
   if (!meta) return 1;
   const char *metaEnd = objectEnd(meta + sizeof META - 2, end);
   const char *title = findIn(meta, metaEnd, TITLE);
   if (title) readTextObject(title, objectEnd(title + sizeof TITLE - 2, metaEnd), item->title, sizeof item->title);
   int line = 0;
   for (const char *at = meta; line < 2 && (at = findIn(at, metaEnd, LINE)) != NULL; line++) {
      const char *lineEnd = objectEnd(at + sizeof LINE - 2, metaEnd);
      const char *text = findIn(at, lineEnd, TEXT);
      if (text) {
         char words[96];
         readTextObject(text, objectEnd(text + sizeof TEXT - 2, lineEnd), words, sizeof words);
         if (endsWith(words, " subscribers") || endsWith(words, " subscriber")) copyText(item->views, sizeof item->views, words);
         else if (words[0] == '@') copyText(item->author, sizeof item->author, words);
      }
      at = lineEnd;
   }
   // (V3.0.0, a diagnosis for the Subscriptions page) once a run: does a channel tile carry the channel's picture? The
   // host and the size part of its first image only - never the path, which names the channel.
   static int pictureLogged;
   if (!pictureLogged) {
      pictureLogged = 1;
      static const char URL_KEY[] = "\"url\":\"https://";
      const char *url = findIn(tile, end, URL_KEY);
      int images = 0;
      for (const char *at = tile; (at = findIn(at, end, URL_KEY)) != NULL; at += sizeof URL_KEY - 1) images++;
      char host[48] = "", size[48] = "";
      if (url) {
         const char *h = url + sizeof URL_KEY - 1, *slash = h;
         while (slash < end && *slash != '/' && *slash != '"' && slash - h < (int)sizeof host - 1) slash++;
         memcpy(host, h, (size_t)(slash - h));
         host[slash - h] = 0;
         const char *quote = slash, *equals = NULL;
         while (quote < end && *quote != '"') { if (*quote == '=') equals = quote; quote++; }
         if (equals && quote - equals - 1 < (int)sizeof size) { memcpy(size, equals + 1, (size_t)(quote - equals - 1)); size[quote - equals - 1] = 0; }
      }
      logInfo("[yt] channel tiles: %d image url(s) in the first, %s%s%s\n", images, url ? host : "no picture",
              size[0] ? ", size " : "", size);
   }
   return 1;
}

// channels: keep the channel tiles (ACCOUNT_FEED_CHANNELS) instead of the videos
static void parseTileResults(const char *resp, int respLen, int channels, SearchResults *out)
{
   const char *end = resp + respLen;
   static const char TILE[] = "\"tileRenderer\":{";
   for (const char *at = resp; out->count < MAX_SEARCH_RESULTS && (at = findIn(at, end, TILE)) != NULL; ) {
      const char *open = at + sizeof TILE - 2, *close = objectEnd(open, end);
      at = close;
      if (channels) {
         SearchResult *item = &out->items[out->count];
         memset(item, 0, sizeof *item);
         if (findIn(open, close, "\"contentType\":\"TILE_CONTENT_TYPE_CHANNEL\"") && parseChannelTile(open, close, item))
            out->count++;
         continue;
      }
      if (!findIn(open, close, "\"contentType\":\"TILE_CONTENT_TYPE_VIDEO\"")) continue;   // Shorts, channels, the ad
      if (findIn(open, close, "Members only")) continue;   // unplayable for the anonymous player, as in parseVideoResults
      SearchResult *item = &out->items[out->count];
      memset(item, 0, sizeof *item);
      if (parseTileItem(open, close, item) && !isDuplicate(out, item->videoId)) out->count++;
   }
   // The next page is the reply's LAST nextContinuationData: on the home feed each shelf carries one for its own row
   // and the list of shelves its own after all of them; everywhere else there is just the one.
   static const char NEXT[] = "\"nextContinuationData\":{";
   const char *last = NULL;
   for (const char *at = resp; (at = findIn(at, end, NEXT)) != NULL; at += sizeof NEXT - 1) last = at;
   if (last) jsonString(last, end, "continuation", out->continuation, sizeof out->continuation);
}

// POST a signed-in TV browse and parse its tiles. A 401 - the access token refused before its time - is answered
// once with a fresh one.
// POST a signed-in TV request. The reply's body comes back in a buffer of its own (the caller frees it), NULL on any
// failure. A 401 - the access token refused before its time - is answered once with a fresh one.
static char *postAccountRaw(const char *url, const char *body, int bodyLen, const char *what, int cap, int *respLen)
{
   char *authorization = (char *)malloc(AUTHORIZATION_CAP);
   char *resp = (char *)malloc(cap);
   if (!authorization || !resp) { free(authorization); free(resp); return NULL; }
   int ok = 0;
   for (int attempt = 0; attempt < 2; attempt++) {
      if (getAccountAuthorization(authorization, AUTHORIZATION_CAP) != 0) {
         logWarn("[yt] account %s: not signed in, or the sign-in could not be renewed\n", what);
         break;
      }
      HttpHeader headers[6] = {
         { "Content-Type", "application/json" },
         { "User-Agent", ACCOUNT_TV_USER_AGENT },
         { "X-YouTube-Client-Name", "7" },
         { "X-YouTube-Client-Version", ACCOUNT_TV_CLIENT_VERSION },
         { "Authorization", authorization },
         { "Accept-Encoding", "identity" },
      };
      int status = 0;
      int rc = fetchHttp("POST", url, headers, 6, body, bodyLen, resp, cap, respLen, &status);
      if (rc < 0) { logError("[yt] account %s: no answer (rc=%d)\n", what, rc); break; }
      if (status == 401 && attempt == 0) { dropAccountAccess(); continue; }
      if (status != 200) { logError("[yt] account %s: HTTP %d\n", what, status); break; }
      ok = 1;
      break;
   }
   memset(authorization, 0, AUTHORIZATION_CAP);
   free(authorization);
   if (!ok) { free(resp); return NULL; }
   return resp;
}

static int postAccountBrowse(const char *body, int bodyLen, const char *what, int nextPage, int channels,
                             SearchResults *out)
{
   int respLen = 0;
   char *resp = postAccountRaw(BROWSE_URL, body, bodyLen, what, RESP_CAP, &respLen);
   if (!resp) return -1;
   parseTileResults(resp, respLen, channels, out);
   logInfo("[yt] account %s %s: %d %s, next page %s\n", what, nextPage ? "next page" : "page 1", out->count,
           channels ? "channels" : "videos", out->continuation[0] ? "yes" : "no");
   free(resp);
   return 0;
}

static int fetchAccountFeed(AccountFeed feed, const char *continuation, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   if (feed < 0 || feed >= ACCOUNT_FEED_COUNT) return -1;
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512];
   int bodyLen = continuation ? snprintf(body, sizeof body, TV_CONTINUATION_BODY_FMT, gl, continuation)
                              : snprintf(body, sizeof body, TV_BROWSE_BODY_FMT, gl, ACCOUNT_BROWSE_IDS[feed]);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   return postAccountBrowse(body, bodyLen, ACCOUNT_BROWSE_IDS[feed], continuation != NULL,
                            feed == ACCOUNT_FEED_CHANNELS, out);
}

// ---- the account's playlists ----

#define PLAYLIST_PAGES 5   // the Library's playlist list is read this many pages deep at the most

// an id as it goes into "VL<id>" and a JSON string: letters, digits, '-' and '_' only, with room for the "VL"
static int isPlaylistId(const char *id)
{
   int length = (int)strlen(id);
   if (length < 2 || length > ACCOUNT_PLAYLIST_ID_CAP - 3) return 0;
   for (const char *p = id; *p; p++) if (!isIdChar(*p)) return 0;
   return 1;
}

// The playlists of one reply of FEplaylist_aggregation, from index `count` on. Returns the new count.
static int parsePlaylistTiles(const char *resp, int respLen, AccountPlaylistInfo *out, int count, int cap)
{
   const char *end = resp + respLen;
   static const char TILE[] = "\"tileRenderer\":{", META[] = "\"tileMetadataRenderer\":{", TITLE[] = "\"title\":{";
   static const char OVERLAY[] = "\"thumbnailOverlayTimeStatusRenderer\":{";
   for (const char *at = resp; count < cap && (at = findIn(at, end, TILE)) != NULL; ) {
      const char *open = at + sizeof TILE - 2, *close = objectEnd(open, end);
      at = close;
      if (!findIn(open, close, "\"contentType\":\"TILE_CONTENT_TYPE_PLAYLIST\"")) continue;
      AccountPlaylistInfo *list = &out[count];
      memset(list, 0, sizeof *list);
      if (!jsonString(open, close, "contentId", list->id, sizeof list->id) || !isPlaylistId(list->id)) continue;
      if (strcmp(list->id, "WL") == 0) continue;   // Watch Later has a page of its own
      int known = 0;
      for (int i = 0; i < count; i++) if (strcmp(out[i].id, list->id) == 0) known = 1;
      if (known) continue;
      const char *meta = findIn(open, close, META);
      if (meta) {
         const char *metaEnd = objectEnd(meta + sizeof META - 2, close);
         const char *title = findIn(meta, metaEnd, TITLE);
         if (title) readTextObject(title, objectEnd(title + sizeof TITLE - 2, metaEnd), list->title, sizeof list->title);
      }
      const char *overlay = findIn(open, close, OVERLAY);
      if (overlay) {
         const char *overlayOpen = overlay + sizeof OVERLAY - 2;
         readTextObject(overlayOpen, objectEnd(overlayOpen, close), list->count, sizeof list->count);
      }
      if (!list->title[0]) strCopy(list->title, sizeof list->title, strcmp(list->id, "LL") == 0 ? "Liked videos" : list->id);
      count++;
   }
   return count;
}

static int fetchAccountPlaylists(AccountPlaylistInfo *out, int cap)
{
   const char *gl = getContentRegionCode();
   char *body = (char *)malloc(MAX_CONTINUATION + 512), *token = (char *)malloc(MAX_CONTINUATION);
   if (!body || !token) { free(body); free(token); return -1; }
   int count = 0, failed = 0;
   token[0] = 0;
   for (int page = 0; page < PLAYLIST_PAGES && count < cap; page++) {
      int bodyLen = token[0] ? snprintf(body, MAX_CONTINUATION + 512, TV_CONTINUATION_BODY_FMT, gl, token)
                             : snprintf(body, MAX_CONTINUATION + 512, TV_BROWSE_BODY_FMT, gl, "FEplaylist_aggregation");
      if (bodyLen < 0 || bodyLen >= MAX_CONTINUATION + 512) break;
      int respLen = 0;
      char *resp = postAccountRaw(BROWSE_URL, body, bodyLen, "playlists", RESP_CAP, &respLen);
      if (!resp) { failed = page == 0; break; }
      count = parsePlaylistTiles(resp, respLen, out, count, cap);
      token[0] = 0;
      static const char NEXT[] = "\"nextContinuationData\":{";
      const char *last = NULL;
      for (const char *at = resp; (at = findIn(at, resp + respLen, NEXT)) != NULL; at += sizeof NEXT - 1) last = at;
      if (last) jsonString(last, resp + respLen, "continuation", token, MAX_CONTINUATION);
      free(resp);
      if (!token[0]) break;
   }
   free(body);
   free(token);
   if (failed) return -1;
   logInfo("[yt] account playlists: %d\n", count);
   return count;
}

static int fetchAccountPlaylistVideos(const char *playlistId, const char *continuation, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   if (!continuation && !isPlaylistId(playlistId)) return -1;
   const char *gl = getContentRegionCode();
   char body[MAX_CONTINUATION + 512], browseId[ACCOUNT_PLAYLIST_ID_CAP + 2];
   snprintf(browseId, sizeof browseId, "VL%s", playlistId);
   int bodyLen = continuation ? snprintf(body, sizeof body, TV_CONTINUATION_BODY_FMT, gl, continuation)
                              : snprintf(body, sizeof body, TV_BROWSE_BODY_FMT, gl, browseId);
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   return postAccountBrowse(body, bodyLen, "playlist", continuation != NULL, 0, out);
}

// ---- changes to the signed-in account ----
//
// What the TV app sends for the same buttons: its Watch Later entries are a playlist edit of "WL" (the tiles' own
// long-press menu carries exactly that playlistEditEndpoint), a subscription is subscription/subscribe with the
// channel's id. YouTube answers both with "STATUS_SUCCEEDED" when it took them.

#define EDIT_PLAYLIST_URL "https://www.youtube.com/youtubei/v1/browse/edit_playlist?prettyPrint=false"
#define SUBSCRIBE_URL     "https://www.youtube.com/youtubei/v1/subscription/subscribe?prettyPrint=false"
#define UNSUBSCRIBE_URL   "https://www.youtube.com/youtubei/v1/subscription/unsubscribe?prettyPrint=false"
#define ACTION_REPLY_CAP  (256 * 1024)

static int isChannelId(const char *id)
{
   if (strncmp(id, "UC", 2) != 0 || strlen(id) != 24) return 0;
   for (const char *p = id; *p; p++) if (!isIdChar(*p)) return 0;
   return 1;
}

static int changeAccountOnYouTube(AccountAction action, const char *id)
{
   int isVideo = action == ACCOUNT_ADD_WATCH_LATER || action == ACCOUNT_REMOVE_WATCH_LATER;
   char videoId[16];
   if (isVideo ? !extractVideoId(id, videoId, sizeof videoId) : !isChannelId(id)) {
      logError("[yt] account change refused: not a %s id\n", isVideo ? "video" : "channel");
      return -1;
   }
   const char *gl = getContentRegionCode(), *url, *what;
   char body[1024];
   if (action == ACCOUNT_ADD_WATCH_LATER) {
      url = EDIT_PLAYLIST_URL; what = "Watch Later add";
      snprintf(body, sizeof body, TV_CONTEXT_FMT ",\"playlistId\":\"WL\",\"actions\":[{\"action\":\"ACTION_ADD_VIDEO\","
               "\"addedVideoId\":\"%s\"}]}", gl, videoId);
   } else if (action == ACCOUNT_REMOVE_WATCH_LATER) {
      url = EDIT_PLAYLIST_URL; what = "Watch Later remove";
      snprintf(body, sizeof body, TV_CONTEXT_FMT ",\"playlistId\":\"WL\",\"actions\":[{\"action\":"
               "\"ACTION_REMOVE_VIDEO_BY_VIDEO_ID\",\"removedVideoId\":\"%s\"}]}", gl, videoId);
   } else {
      url = action == ACCOUNT_SUBSCRIBE ? SUBSCRIBE_URL : UNSUBSCRIBE_URL;
      what = action == ACCOUNT_SUBSCRIBE ? "subscribe" : "unsubscribe";
      snprintf(body, sizeof body, TV_CONTEXT_FMT ",\"channelIds\":[\"%s\"]}", gl, id);
   }

   char *authorization = (char *)malloc(AUTHORIZATION_CAP);
   char *resp = (char *)malloc(ACTION_REPLY_CAP);
   if (!authorization || !resp) { free(authorization); free(resp); return -1; }
   int rc = -1;
   for (int attempt = 0; attempt < 2; attempt++) {
      if (getAccountAuthorization(authorization, AUTHORIZATION_CAP) != 0) {
         logWarn("[yt] account %s: not signed in, or the sign-in could not be renewed\n", what);
         break;
      }
      HttpHeader headers[6] = {
         { "Content-Type", "application/json" },
         { "User-Agent", ACCOUNT_TV_USER_AGENT },
         { "X-YouTube-Client-Name", "7" },
         { "X-YouTube-Client-Version", ACCOUNT_TV_CLIENT_VERSION },
         { "Authorization", authorization },
         { "Accept-Encoding", "identity" },
      };
      int respLen = 0, status = 0;
      rc = fetchHttp("POST", url, headers, 6, body, (int)strlen(body), resp, ACTION_REPLY_CAP, &respLen, &status);
      if (rc < 0) { logError("[yt] account %s: no answer (rc=%d)\n", what, rc); break; }
      if (status == 401 && attempt == 0) { dropAccountAccess(); continue; }
      int took = status == 200 && findIn(resp, resp + respLen, "STATUS_SUCCEEDED") != NULL;
      // a subscribe answers with the button's new state rather than a status: 200 without an error is taken
      if (status == 200 && !took && !isVideo && !findIn(resp, resp + respLen, "\"error\"")) took = 1;
      logInfo("[yt] account %s: HTTP %d, %s\n", what, status, took ? "done" : "not taken");
      rc = took ? 0 : -1;
      break;
   }
   memset(authorization, 0, AUTHORIZATION_CAP);
   free(authorization);
   free(resp);
   return rc;
}

// ---- the Live page ----

#define LIVE_BROWSE_ID     "UC4R8DWoMoI7CAwX8_LjQHig"   // YouTube's own Live destination, what youtube.com/live shows
#define LIVE_SEARCH_PARAMS "EgJAAQ=="                    // the search filter "Live" (protobuf 12 02 40 01)
#define LIVE_SESSION_CAP   96                            // a session is 44 characters

typedef enum { LIVE_KIND_BROWSE, LIVE_KIND_SEARCH } LiveKind;

// The chips' sources, in LiveCategory order (keep in sync). Popular, Gaming, Sports and News are shelves of the Live
// destination - "Live Now", "Live Now - Gaming" and so on; params = "livetab" plus the shelf's number - whose next
// pages come only with page 1's session echoed back. Podcasts and Music are live searches, which page without one.
// A shelf whose page 1 has fewer live streams than fallbackBelow is topped up by a live search: Popular, Gaming and
// Sports only if their shelf ever comes back empty (YouTube can retire a shelf, as it retired FEtrending); News
// whenever it is thin - it is empty or holds a few streams in many regions - and with the region's own word for
// news (fallbackQuery NULL, see newsWord).
static const struct {
   LiveKind    kind;
   const char *name;            // for the log
   const char *idOrQuery;       // the browseId, or the search query
   const char *params;
   const char *fallbackQuery;
   int         fallbackBelow;
} LIVE_FEEDS[LIVE_COUNT] = {
   { LIVE_KIND_BROWSE, "popular",  LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKEK", "live",   1 },   // LIVE_POPULAR: shelf 1313
   { LIVE_KIND_BROWSE, "gaming",   LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCJ8K", "gaming", 1 },   // LIVE_GAMING: 1311
   { LIVE_KIND_BROWSE, "sports",   LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKAK", "sports", 1 },   // LIVE_SPORTS: 1312
   { LIVE_KIND_SEARCH, "podcasts", "podcast",      LIVE_SEARCH_PARAMS,     NULL,     0 },   // LIVE_PODCASTS
   { LIVE_KIND_BROWSE, "news",     LIVE_BROWSE_ID, "EgdsaXZldGFikgEDCKUK", NULL,     6 },   // LIVE_NEWS: 1317
   { LIVE_KIND_SEARCH, "music",    "music",        LIVE_SEARCH_PARAMS,     NULL,     0 },   // LIVE_MUSIC
};

// The word a viewer in each region would search news with, for the News chip's fallback: an English "news" search
// finds NBC, CBS and Times Now from anywhere (probed: RU "новости" gave 13 Russian streams of 20, DE "nachrichten"
// put WELT first). UTF-8, written as escapes so this file stays ASCII; a literal is split where the next letter
// would read as part of a hex escape. jsonEscape passes the bytes through as they are, which JSON allows. Codes
// not listed search "news"; a code the region table no longer has never gets here.
static const struct { const char *codes, *word; } NEWS_WORDS[] = {
   { "DE AT CH", "nachrichten" },
   { "FR",       "actualit\xc3\xa9" "s" },                                               // actualités
   { "IT",       "notizie" },
   { "ES MX",    "noticias" },
   { "PT BR",    "not\xc3\xad" "cias" },                                                 // notícias
   { "NL BE",    "nieuws" },
   { "PL",       "wiadomo\xc5\x9b" "ci" },                                               // wiadomości
   { "CZ",       "zpr\xc3\xa1" "vy" },                                                   // zprávy
   { "TR",       "haber" },
   { "RU",       "\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x81\xd1\x82\xd0\xb8" },           // новости
   { "UA BG",    "\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd0\xbd\xd0\xb8" },                   // новини
   { "RS",       "\xd0\xb2\xd0\xb5\xd1\x81\xd1\x82\xd0\xb8" },                           // вести
   { "HR",       "vijesti" },
   { "GR",       "\xce\xb5\xce\xb9\xce\xb4\xce\xae\xcf\x83\xce\xb5\xce\xb9\xcf\x82" },   // ειδήσεις
   { "SE NO",    "nyheter" },
   { "DK",       "nyheder" },
   { "FI",       "uutiset" },
   { "HU",       "h\xc3\xad" "rek" },                                                    // hírek
   { "RO",       "\xc8\x99" "tiri" },                                                    // știri
   { "JP",       "\xe3\x83\x8b\xe3\x83\xa5\xe3\x83\xbc\xe3\x82\xb9" },                   // ニュース
   { "KR",       "\xeb\x89\xb4\xec\x8a\xa4" },                                           // 뉴스
};

static const char *newsWord(const char *gl)
{
   for (unsigned i = 0; i < sizeof NEWS_WORDS / sizeof NEWS_WORDS[0]; i++)
      for (const char *code = NEWS_WORDS[i].codes; code[0] && code[1]; code += code[2] ? 3 : 2)
         if (code[0] == gl[0] && code[1] == gl[1]) return NEWS_WORDS[i].word;
   return "news";
}

// A live next-page token says which kind of request it continues, and the next page goes wherever that says -
// never guessed from whether a session is present: "b|<session>|<token>" continues a shelf, whose YouTube token is
// bound to the session of the reply that handed it out (sent back as X-Goog-Visitor-Id), and "s|<token>" continues
// a live search. '|' occurs in neither a token nor a session. The grid stores and returns it untouched.

// Turns the YouTube token in `token` into a live token in place: a shelf's when session is set, a search's when it
// is NULL. 0 done; -1, with the token cleared so the list ends here, when it would not fit or cannot be wrapped.
static int wrapLiveToken(char *token, int cap, const char *session)
{
   int tokenLength = (int)strlen(token);
   int sessionLength = session ? (int)strlen(session) : 0;
   int prefix = session ? 3 + sessionLength : 2;
   if (strchr(token, '|') || tokenLength + prefix >= cap) { token[0] = 0; return -1; }
   memmove(token + prefix, token, tokenLength + 1);   // no 4 KB copy on the 64 KB worker stack
   token[0] = session ? 'b' : 's';
   token[1] = '|';
   if (session) { memcpy(token + 2, session, sessionLength); token[2 + sessionLength] = '|'; }
   return 0;
}

// The YouTube token inside a live token, with *browse set and - for a shelf - its session copied into session.
// NULL for anything that is not a well-formed live token, including a session that would not pass as a header.
static const char *splitLiveToken(const char *continuation, int *browse, char *session, int cap)
{
   session[0] = 0;
   if (continuation[0] == 's' && continuation[1] == '|') {
      *browse = 0;
      return continuation[2] ? continuation + 2 : NULL;
   }
   if (continuation[0] != 'b' || continuation[1] != '|') return NULL;
   const char *start = continuation + 2, *bar = strchr(start, '|');
   int length = bar ? (int)(bar - start) : 0;
   if (length <= 0 || length >= cap || !bar[1]) return NULL;
   memcpy(session, start, length);
   session[length] = 0;
   if (!isSessionToken(session)) { session[0] = 0; return NULL; }
   *browse = 1;
   return bar + 1;
}

// Live streams only, from index `from` on: drops the stream that ended seconds before the shelf was built (it comes
// back as a video with a duration) and anything upcoming. Compacts in place; returns how many are left from `from`.
static int keepLiveOnly(SearchResults *out, int from)
{
   int kept = from;
   for (int i = from; i < out->count; i++) {
      if (!out->items[i].isLive) continue;
      if (kept != i) out->items[kept] = out->items[i];
      kept++;
   }
   out->count = kept;
   return kept - from;
}

static int fetchLive(LiveCategory category, const char *continuation, SearchResults *out, int (*stopped)(void))
{
   if (category < 0 || category >= LIVE_COUNT) { memset(out, 0, sizeof *out); return -1; }
   const char *gl = getContentRegionCode();   // read once: every request of this call uses this one value
   const char *name = LIVE_FEEDS[category].name;
   char body[MAX_CONTINUATION + 512];
   char sent[LIVE_SESSION_CAP] = "", reply[LIVE_SESSION_CAP] = "";
   char escaped[256];
   int browse = LIVE_FEEDS[category].kind == LIVE_KIND_BROWSE;
   int bodyLen;
   WebRequest request = { NULL, NULL, NULL, 0, 0 };

   if (!continuation) {
      jsonEscape(LIVE_FEEDS[category].idOrQuery, escaped, sizeof escaped);
      if (browse) bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, gl, escaped, LIVE_FEEDS[category].params);
      else        bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, escaped, LIVE_FEEDS[category].params);
      request.pageOneGl = gl;
   } else {
      const char *token = splitLiveToken(continuation, &browse, sent, sizeof sent);
      if (!token) {   // not ours: end the list quietly rather than send YouTube something it cannot answer
         logError("[yt] live %s: not a live next-page token, the list ends here\n", name);
         memset(out, 0, sizeof *out);
         return 0;
      }
      bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, gl, token);
      if (browse) request.visitorId = sent;
   }
   if (bodyLen < 0 || bodyLen >= (int)sizeof body) return refuseBody(bodyLen, out);
   if (browse) { request.replyVisitor = reply; request.replyVisitorCap = sizeof reply; }

   int rc = postAndParseWith(browse ? BROWSE_URL : SEARCH_URL, body, bodyLen, &request, out);
   int fallbackBelow = continuation ? 0 : LIVE_FEEDS[category].fallbackBelow;
   // a shelf YouTube has retired may answer with an HTTP error instead of an empty page (FEtrending now says 400):
   // a page 1 that has a fallback goes on to it; a lost connection (rc above -100) fails as every list does
   if (rc < 0 && (fallbackBelow == 0 || rc > -100)) {
      logError("[yt] live %s gl=%s %s failed rc=%d\n", name, gl, continuation ? "next page" : "page 1", rc);
      return rc;
   }
   char note[128] = "";   // what else happened, for the log line
   if (rc < 0) snprintf(note, sizeof note, ", the shelf answered HTTP %d", -rc);
   int parsed = out->count;
   int live = keepLiveOnly(out, 0);   // the shelf's own count, logged as such
   int tokenIsBrowse = browse;

   if (live < fallbackBelow) {
      const char *word = LIVE_FEEDS[category].fallbackQuery ? LIVE_FEEDS[category].fallbackQuery : newsWord(gl);
      int noted = (int)strlen(note);
      if (stopped && stopped()) {   // the page was switched away from: its result is thrown away anyway
         snprintf(note + noted, sizeof note - noted, ", stopped before the fallback search");
      } else {
         jsonEscape(word, escaped, sizeof escaped);
         bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, gl, escaped, LIVE_SEARCH_PARAMS);
         WebRequest fallback = { gl, NULL, NULL, 0, 1 };
         int before = out->count;
         int fallbackRc = (bodyLen >= 0 && bodyLen < (int)sizeof body) ? postAndParseWith(SEARCH_URL, body, bodyLen, &fallback, out) : -1;
         if (fallbackRc == 0) {
            // the shelf's own streams first, the search's after them; the list pages on with the search's token
            int added = keepLiveOnly(out, before);
            snprintf(note + noted, sizeof note - noted, ", fallback search \"%s\": +%d live", word, added);
            tokenIsBrowse = 0;
            rc = 0;
         } else {   // out is as the shelf left it, its own next-page token included
            snprintf(note + noted, sizeof note - noted, ", fallback search \"%s\" failed rc=%d", word, fallbackRc);
         }
      }
      if (rc < 0) {   // neither the shelf nor its fallback answered
         logError("[yt] live %s gl=%s page 1 failed%s\n", name, gl, note);
         return rc;
      }
   }

   // every reply brings a new session, and the next page wants this one; a reply without one leaves the one sent
   // in force. A shelf token with no session would only fetch an empty page, so the list ends instead (echo=0).
   const char *session = reply[0] ? reply : sent;
   if (tokenIsBrowse && !session[0]) out->continuation[0] = 0;
   int tooLong = out->continuation[0] && wrapLiveToken(out->continuation, sizeof out->continuation, tokenIsBrowse ? session : NULL) != 0;
   logInfo("[yt] live %s gl=%s %s: parsed %d, live %d%s, next=%d%s%s\n", name, gl, continuation ? "next page" : "page 1",
           parsed, live, note, out->continuation[0] ? 1 : 0,
           tokenIsBrowse ? (session[0] ? ", echo=1" : ", echo=0") : "", tooLong ? ", token too long - the list ends" : "");
   return 0;
}

const Extractor youtubeExtractor = { "youtube", matches, extract, search, fetchTrending, getChannel, fetchLive,
                                     fetchAccountFeed, changeAccountOnYouTube, fetchAccountPlaylists,
                                     fetchAccountPlaylistVideos };

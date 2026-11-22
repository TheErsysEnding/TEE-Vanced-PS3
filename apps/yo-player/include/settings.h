#pragma once

// settings - user-editable options in /dev_hdd0/tmp/yo-player/settings.txt. the file is created
// with defaults and explanatory comments on first launch, so it can be edited over FTP; an unknown
// or malformed value silently falls back to its default. loadSettings() must run once at startup.

#include <stdint.h>   // (V3.0.3) the subtitle style's colours

// when auto-skip acts on community-flagged SponsorBlock segments.
typedef enum {
   SPONSORBLOCK_OFF,   // never skip anything (the segments aren't even fetched)
   SPONSORBLOCK_ADS,   // skip paid promotions only: sponsors, self-promo, like/subscribe reminders
   SPONSORBLOCK_ALL    // also skip intros, outros, filler and non-music sections
} SponsorblockMode;

void loadSettings(void);   // reads settings.txt, creating it with defaults when missing

SponsorblockMode getSponsorblockMode(void);

// settings.txt video-decoder=auto|sony: the TEE Decoder where it can decode the video (auto, the default) or
// Sony's cellVdec only; and tee-decoder-spes=4|5, the TEE Decoder's SPEs (1 CABAC + 3 or 4 pixel).
typedef enum { VIDEO_DECODER_AUTO, VIDEO_DECODER_SONY } VideoDecoderSetting;
VideoDecoderSetting getVideoDecoderSetting(void);
int getTeeDecoderSpes(void);
// settings.txt vp9-spes=6|5: the TEE VP9 building block's SPEs for 1440p - 6 (2 + 4, no sound) or 5 (1 + 4, sound)
int getVp9Spes(void);
// (V3.1.0 Test 3) settings.txt display-sync=flips|wall (1 / 0, see setVideoDisplaySync), log=async|sync (1 / 0: the log
// written by a thread of its own, startLogWriter), vp9-release=early|late (1 / 0, setVideoVp9ReleaseEarly)
int getDisplaySyncByFlips(void);
int getLogAsync(void);
int getVp9ReleaseEarly(void);
// (V3.1.0 Test 4) experimental - may freeze the console, so not in the file it writes; only with someone at the console:
// gfx-share=off|on (0 / 1): the RSX's command buffer shared with TEE VP9's RSX part (gfx.h setGfxShareMode - main.c reads it
// with readEarlySetting before initGfx, a bench job's global gfx-share= winning); vp9-rsx=off|1440|on (0 / 1 / 2): TEE VP9
// predicts on the RSX, up to 1440p or 4K too (needs gfx-share=on); rsx-test=<word>: a test of its failure paths ("" none)
int getGfxShare(void);
int getVp9Rsx(void);
const char *getRsxTest(void);
// key= from settings.txt before loadSettings has run: 1 with its first word in value (cut to cap - 1 bytes), 0 without it
int readEarlySetting(const char *key, char *value, int cap);
// (V3.1.0) settings.txt language=<code>: the menu's language (i18n.h). 1 with the code when one was chosen, 0 at the
// first start (the picker asks). saveLanguageSetting writes it: 0 saved, -1 not (UI thread).
int getLanguageSetting(char *code, int cap);
int saveLanguageSetting(const char *code);
// settings.txt download-quality=1440|1080|xmb: the R3 page's last choice (0, 1, 2 - downloads.h DownloadQuality)
int getDownloadQuality(void);
int setDownloadQuality(int quality);

// settings.txt quality=auto|<height>: the highest video height to play (0 = auto, the best that plays smoothly).
// setQualityCap changes it and saves it (0 saved, -1 not saved - the cap still applies for this run).
int getQualityCap(void);
int getUpdateCheck(void);   // (V3.0.0 Test 12) settings.txt update-check=: 1 on (the default), 0 off (update-check.h)
int setQualityCap(int height);

// ---- content region ----
// Which country's YouTube the lists come from: the "gl" of every WEB request (Trending, Live, Search, channel
// pages and their next pages). The language ("hl") stays English whatever the region, because the parsers read
// English text (" views", " ago", " watching", "Members only"). Kept in settings.txt as region=<code>, changed
// in Settings > Region. The default is the United States, which is what every earlier build sent. Only codes
// from the fixed table are ever sent: YouTube answers an unknown or lower-case gl with an empty page, not an
// error.

// One country the lists come from: code = YouTube's gl (upper case), name = English display name, language = the
// code YouTube's subtitle translation takes for its main language (V3.0.0, see getSubtitleLanguage).
typedef struct { const char *code; const char *name; const char *language; } ContentRegion;
int  getContentRegionCount(void);

// ---- subtitles (V3.0.0, GitHub issue #4) ----
// The language a video's subtitles are translated into when the video has none in it - YouTube's "auto-translated"
// tracks. settings.txt subtitle-language=: auto (the default since V3.0.3) = the content region's language (Czechia:
// cs), a code such as cs, pl, sk, hu, de, en, or off. "" when off; never NULL. YouTube refuses its own translations
// to an anonymous player without a PO token (HTTP 429, probed 2026-11-12 and 13), so the player translates the
// track itself with Google Translate (subtitle-translate.h).
const char *getSubtitleLanguage(void);

// ---- the subtitle style (V3.0.3, asked for on PSX-Place) ----
// How subtitles look: the player's START > Subtitle style, kept in settings.txt as subtitle-color=, subtitle-background=,
// subtitle-opacity=, subtitle-edge= and subtitle-size=. Each field is one of a few choices, by index.
typedef enum { SUBTITLE_STYLE_COLOR, SUBTITLE_STYLE_BACKGROUND, SUBTITLE_STYLE_OPACITY, SUBTITLE_STYLE_EDGE,
               SUBTITLE_STYLE_SIZE, SUBTITLE_STYLE_FIELDS } SubtitleStyleField;
typedef enum { SUBTITLE_EDGE_NONE, SUBTITLE_EDGE_SHADOW, SUBTITLE_EDGE_OUTLINE } SubtitleEdge;
int          getSubtitleStyleChoice(SubtitleStyleField field);                  // the index in use
int          getSubtitleStyleChoiceCount(SubtitleStyleField field);
const char  *getSubtitleStyleChoiceName(SubtitleStyleField field, int choice);  // "Yellow", "80%", "Shadow", "125%"
void         setSubtitleStyleChoice(SubtitleStyleField field, int choice);      // in use at once, saved by saveSubtitleStyle
int          saveSubtitleStyle(void);   // UI thread: the fields changed since the last save into settings.txt; 0 saved
uint32_t     getSubtitleTextColor(void);         // 0xFFRRGGBB
uint32_t     getSubtitleBackgroundColor(void);   // 0xAARRGGBB, the opacity as alpha: 0 = no plate
SubtitleEdge getSubtitleEdge(void);
int          getSubtitleTextSize(void);          // in px: 30 at 100%
const ContentRegion *getContentRegionAt(int index);   // the fixed table in display order; NULL out of range
int  getContentRegionIndex(void);                     // the current region's index into that table
const char *getContentRegionCode(void);               // the current gl, e.g. "DE"; never NULL, always from the table
int  setContentRegionIndex(int index);                // UI thread only, while no list is loading; saves region= in
                                                      // settings.txt; 0 saved, -1 not saved (the region still changes)
unsigned getContentRegionRevision(void);              // +1 on every change, for caches to notice

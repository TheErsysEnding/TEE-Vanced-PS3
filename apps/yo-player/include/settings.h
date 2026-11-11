#pragma once

// settings - user-editable options in /dev_hdd0/tmp/yo-player/settings.txt. the file is created
// with defaults and explanatory comments on first launch, so it can be edited over FTP; an unknown
// or malformed value silently falls back to its default. loadSettings() must run once at startup.

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

// settings.txt quality=auto|<height>: the highest video height to play (0 = auto, the best that plays smoothly).
// setQualityCap changes it and saves it (0 saved, -1 not saved - the cap still applies for this run).
int getQualityCap(void);
int setQualityCap(int height);

// ---- content region ----
// Which country's YouTube the lists come from: the "gl" of every WEB request (Trending, Live, Search, channel
// pages and their next pages). The language ("hl") stays English whatever the region, because the parsers read
// English text (" views", " ago", " watching", "Members only"). Kept in settings.txt as region=<code>, changed
// in Settings > Region. The default is the United States, which is what every earlier build sent. Only codes
// from the fixed table are ever sent: YouTube answers an unknown or lower-case gl with an empty page, not an
// error.

// One country the lists come from: code = YouTube's gl (upper case), name = English display name.
typedef struct { const char *code; const char *name; } ContentRegion;
int  getContentRegionCount(void);
const ContentRegion *getContentRegionAt(int index);   // the fixed table in display order; NULL out of range
int  getContentRegionIndex(void);                     // the current region's index into that table
const char *getContentRegionCode(void);               // the current gl, e.g. "DE"; never NULL, always from the table
int  setContentRegionIndex(int index);                // UI thread only, while no list is loading; saves region= in
                                                      // settings.txt; 0 saved, -1 not saved (the region still changes)
unsigned getContentRegionRevision(void);              // +1 on every change, for caches to notice

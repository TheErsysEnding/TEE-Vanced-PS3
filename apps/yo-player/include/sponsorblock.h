#pragma once

// sponsorblock - fetches community skip segments for a youtube video from the SponsorBlock API
// (sponsor.ajay.app) and exposes them for auto-skip during playback and colour marking on the seek
// bar. Blocking HTTPS; call fetchSponsorSegments off the UI thread. See sponsorblock.c.

#define MAX_SPONSOR_SEGMENTS 64

// the skip-type categories we auto-skip. order is only local; the API name <-> enum map lives in the .c.
typedef enum {
   SPONSOR_SPONSOR,
   SPONSOR_SELFPROMO,
   SPONSOR_INTERACTION,
   SPONSOR_INTRO,
   SPONSOR_OUTRO,
   SPONSOR_MUSIC_OFFTOPIC,
   SPONSOR_FILLER,
   SPONSOR_CATEGORY_COUNT
} SponsorCategory;

typedef struct {
   float           start, end;   // seconds
   SponsorCategory category;
   int             skipped;      // one-shot guard: set once auto-skipped so a manual rewind won't loop
} SponsorSegment;

typedef struct {
   SponsorSegment segments[MAX_SPONSOR_SEGMENTS];
   int            count;
} SponsorSegments;

// blocking HTTPS GET + parse for a bare 11-char videoId. fills out (out->count 0 on none / failure).
// returns 0 when the request completed (even with zero segments), negative on a transport error.
int fetchSponsorSegments(const char *videoId, SponsorSegments *out);

// ARGB, opaque: the category's colour on the seek bar span, in the settings and in the player's legend
unsigned    getSponsorCategoryColor(SponsorCategory category);
// the category's full name - "Sponsor", "Self-promotion", "Interaction reminder", "Intro", "Outro and end
// cards", "Off-topic music", "Filler and tangents" - for the settings rows, the "Skip ..." offer and the
// "Skipped ..." toast (at most 20 characters, so "Skipped " + name fits the player's 64-byte notices)
const char *getSponsorCategoryName(SponsorCategory category);

#pragma once
// video-details - what a feed item does not say, for Home's list (V3.0.0 Test 15): the best picture the player
// plays of it (height and frame rate, "1440p60" - no codec), what the quality setting makes of that, its
// description, and whether it has subtitles or more than one audio track. Only the player's own resolve knows
// these, so each video is resolved once - on a thread of its own, one video at a time - and kept: the last
// DETAILS_SLOTS of them. UI thread only, apart from the resolve.

#define DETAILS_DESCRIPTION_CAP 1024   // the start of the description: what the details panel has room for

typedef struct {
   char videoId[16];
   int  failed;                    // the resolve did not work: nothing below is known
   int  bestHeight, bestFps;       // the best the player plays (0 = none); height = the picture's shorter side
   int  playHeight, playFps;       // what it plays with the quality setting (START > Quality)
   int  qualityCap;                // that setting when it was resolved (0 = Auto)
   int  isLive, liveDvr;           // on air now, and whether it can be rewound
   int  captions, audioTracks;     // subtitle languages, audio tracks (0 or 1 = one)
   int  durationSeconds;
   int  descriptionCut;            // the description goes on past what is kept
   char description[DETAILS_DESCRIPTION_CAP];
   unsigned used;
} VideoDetails;

// what is known of videoId: NULL until it has been resolved
const VideoDetails *findVideoDetails(const char *videoId);
// 1 while videoId is being resolved
int  isVideoDetailsPending(const char *videoId);

// UI thread, every frame: adopts a finished resolve, then - when none is running - starts the first of wanted[]
// (the selection first, then what is near it) that is not known yet. count 0 starts nothing new.
void updateVideoDetails(const char *const *wanted, int count);

// "1440p60", "1080p", "" (none): a height and frame rate as the list shows them
void formatVideoQuality(char *out, int cap, int height, int fps);

void stopVideoDetails(void);   // joins a resolve in flight (before playback); its video is resolved again later

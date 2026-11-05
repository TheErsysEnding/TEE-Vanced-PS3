// play screen - resolve a video, then stream it with the simple-lib-av engine.
//
// Both stream urls are handed straight to the player: simple-lib-av opens each through
// openHttpStream (the http module), so each demuxer reads the moov and then each sample
// on demand by HTTP range - nothing is downloaded in full. Adaptive picks are split: a
// video-only stream plus an audio-only stream, each its own independent http stream.
//
// Follows file-manager's video overlay: the open work runs on a worker
// (createVideoPlayer does blocking network I/O and must stay off the UI thread),
// the UI thread adopts the built player and pulls frames each render frame.

#include "screens/play.h"
#include "extractor.h"
#include "stream-select.h"   // pickBestVideo / pickBestAudio (shared with download)

#include "gfx.h"
#include "theme.h"              // every colour, the letterbox's included
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/volume-meter.h"
#include "button-repeat.h"     // left/right scrub auto-repeat
#include "ui-fonts.h"           // the shared Manrope weights (and FONT_POP for subtitles), text placement
#include "ui-draw.h"            // gradients, rounded boxes, rings, circles, fixed-width digits, the safe area
#include "ui-hints.h"           // the HUD's hint row and the overlays' own buttons
#include "thread.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy

#include "video-player.h"
#include "audio.h"              // setAudioPcmFeedVolume
#include "storage.h"            // resume position (get/setWatchedPosition)
#include "settings.h"           // sponsorblock-mode (which segment categories auto-skip)
#include "sponsorblock.h"       // skip segments (auto-skip + seek-bar marks)
#include "subtitles.h"          // fetch + parse a caption track; cue lookup during playback
#include "chapters.h"           // description timestamp lines -> chapter list
#include "ps-button.h"          // L3 presses PS for pads the console has no PS button from
#include "ui/console-glyphs.h" // the Triangle glyph on the skip prompt
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)

// volume: up/down step the library's VolumeMeter (ui/volume-meter.h), which keeps the input, its repeat and
// the level. Its look - fifteen pills and a bare white number - is not drawn any more: drawVolume shows the
// level as a fill bar with an outlined percentage on a plate, for as long as the meter would have shown.
#define VOLUME_DEFAULT    12
#define VOLUME_NUMBER_WEIGHT UI_WEIGHT_BOLD   // the meter still renders its own number on a step, never drawn now
#define VOLUME_VISIBLE_US 2500000ULL          // the meter's hide delay (private to volume-meter.c), so the
                                              // plate goes exactly when the meter would have

// seek controls (bar + time) show on activity / pause, then auto-hide. left/right scrub the target
// instantly; the engine seek (a decoder flush + fragment reload each time) fires once input goes quiet.
#define SEEK_STEP_SECONDS   5.0f
#define SEEK_APPLY_IDLE_US  400000ULL
// R2/L2 scrub the same way in minute steps, for crossing a long video without holding a direction for ages
#define FAST_SEEK_STEP_SECONDS 60.0f
// L1 this far into a chapter restarts that chapter instead of stepping back one, the way music players do
#define CHAPTER_REWIND_GRACE   3.0f
#define CONTROLS_VISIBLE_US 3000000ULL
#define TOAST_VISIBLE_US    2000000ULL   // how long a toast ("Skipped ...", "Subtitles: ...") stays up

// ---- the HUD: Bild 2 (2-player.html), in design px on the 1920x1080 canvas ----
// Everything the controls bring up hangs off the safe area (ui-draw.h) and sits on the design's two dark
// gradients. A gradient alone does not carry white text over a white picture. The top one has faded to mid
// grey where Bild 2 puts the meta line and the title, so those two sit on plates of the chips' fill (chips and
// panels bring their own); the text on the bottom gradient (chapter, times, legend) gets a dark outline.
#define HUD_X              UI_SAFE_X               // 96
#define HUD_RIGHT          UI_SAFE_RIGHT           // 1824
#define HUD_W              (HUD_RIGHT - HUD_X)     // 1728
#define TOP_GRADIENT_H     260                     // .78 at the top edge -> clear
#define BOTTOM_GRADIENT_Y  620                     // clear -> .70 at 45 % -> .94 at the bottom edge
#define TEXT_OUTLINE       2                       // px of dark outline round text drawn onto the video

// top left: "channel · views" from the list entry (the resolve has neither, so a typed link has no such
// line) on a chip in the row of the top-right chips, and the title on a plate of the same fill under it.
// Straight on the gradient, as Bild 2 has them, they sat on mid grey over a white picture, with the
// picture's own text running through them.
#define META_SIZE           22
#define META_CHANNEL_WEIGHT UI_WEIGHT_BOLD
#define META_VIEWS_WEIGHT   UI_WEIGHT_REGULAR
#define META_GAP            14
#define CHANNEL_MAX_W       440   // the meta chip ends left of the widest chip row the top right can hold
#define VIEWS_MAX_W         220
#define TITLE_SIZE          44
#define TITLE_WEIGHT        UI_WEIGHT_EXTRABOLD
#define TITLE_MAX_W         1100
#define TITLE_PLATE_Y       (CHIP_Y + CHIP_H + CHIP_GAP)        // 119: under the chip row, so a long title
                                                                // can never run into a chip
#define TITLE_PLATE_H       70    // the title's capitals centred, with room under them for its descenders
#define TITLE_PLATE_BOTTOM  (TITLE_PLATE_Y + TITLE_PLATE_H)     // 189
#define TITLE_PLATE_PAD_X   CHIP_PAD_X       // 16, so the title starts where the meta line does
#define TITLE_PLATE_RADIUS  UI_RADIUS_CARD   // 16

// top right: a chip for each thing that is known - quality, the audio track (several tracks only),
// subtitles (when the video has any)
#define CHIP_Y              64
#define CHIP_H              43
#define CHIP_PAD_X          16
#define CHIP_GAP            12
#define CHIP_RADIUS         10
#define CHIP_TEXT_SIZE      20
#define CHIP_TEXT_WEIGHT    UI_WEIGHT_BOLD
#define AUDIO_CHIP_RADIUS   12     // the radius of ring-r20-w3 at AUDIO_RING_SCALE, so the ring follows it
#define AUDIO_RING_SCALE    0.6f   // a 1.8-px ring: Bild 2's 2-px inset ring, as near as the texture goes
#define AUDIO_RING_ALPHA    0x99   // the ring is the accent at 60 %
#define AUDIO_NAME_MAX_W    340
#define SUBS_CHIP_MAX_W     360

// above the timeline, "row A": the chapter name and "· Chapter n of m" on the left, the time (or LIVE) on
// the right
#define ROW_A_TOP           856    // what subtitles keep clear of while the HUD is up
#define ROW_A_BASELINE      882
#define ROW_A_SIZE          24
#define CHAPTER_NAME_WEIGHT UI_WEIGHT_BOLD
#define CHAPTER_INFO_WEIGHT UI_WEIGHT_SEMIBOLD
#define CHAPTER_NAME_MAX_W  1000   // ends in "..." past this (titles are 63 bytes at most, so it rarely does)
#define ROW_A_GAP           34     // between the chapter block, a legend that moved up here, and the time
#define LIVE_BADGE_Y        (ROW_A_BASELINE - 24)   // the 30-px badge centred on row A's capitals

// the timeline: a piece per chapter with 5-px gaps (one piece without chapters), the knob on top
#define TRACK_Y             916
#define TRACK_H             8
#define TRACK_RADIUS        4
#define KNOB_CY             920
#define KNOB_D              26
#define KNOB_HALO_D         38
#define KNOB_GLOW_SCALE     0.583f // schein-r23 spread to about 120 px round the 26-px knob
#define SPONSOR_MIN_W       4      // a segment shorter than this on the bar still shows as a mark

// the hint row, with the SponsorBlock legend at its right end - or above the timeline when it has no room
// there (layoutLegend)
#define HINT_ROW_Y          952    // the glyphs' top: the row is centred on y 969, as in Bild 2
#define HINT_ROW_GAP        34     // Bild 2's gap between hints
#define HINT_ROW_MIN_GAP    16     // how far it may close up before it would have to give up width
#define LEGEND_SIZE         20
#define LEGEND_WEIGHT       UI_WEIGHT_SEMIBOLD
#define LEGEND_DOT          10
#define LEGEND_DOT_GAP      8
#define LEGEND_ITEM_GAP     18
#define LEGEND_GAP          34     // the last hint -> the legend
#define LEGEND_LINE_TOP     826    // the legend's own line above row A, when neither row has room for it
#define LEGEND_LINE_BASELINE 846

// the times on row A, in fixed-width digits (ui-draw.h) so they do not twitch as they count: the position
// 24/700, " / total" 24/600 as in Bild 2. The percentage on the volume plate uses the position's digits.
#define TIME_TEXT_SIZE     24
#define TIME_TEXT_WEIGHT   UI_WEIGHT_BOLD
#define TOTAL_TEXT_WEIGHT  UI_WEIGHT_SEMIBOLD   // also the start times in the chapter list

// toasts: a pill under the chips at the top right, level with the title plate - or under the plate, when it
// would reach the plate
#define TOAST_TEXT_SIZE    22
#define TOAST_TEXT_WEIGHT  UI_WEIGHT_BOLD
#define TOAST_H            56
#define TOAST_PAD_X        24
#define TOAST_RADIUS       UI_RADIUS_PANEL                   // 28: a pill at this height
#define TOAST_Y            TITLE_PLATE_Y                     // 119
#define TOAST_Y_LOW        (TITLE_PLATE_BOTTOM + CHIP_GAP)   // 201
#define TOAST_TITLE_GAP    32
#define TOAST_MAX_WIDTH    1000   // ends in "..." past this: a long audio-track name used to be cut off bare

// the volume plate on the left, between the stats panel and the bottom block
#define VOLUME_PLATE_Y      392
#define VOLUME_PLATE_W      96
#define VOLUME_PLATE_H      376
#define VOLUME_NUMBER_BOX_Y (VOLUME_PLATE_Y + 22)   // the percentage's capitals centred in this 36-px box
#define VOLUME_NUMBER_BOX_H 36
#define VOLUME_BAR_W        12
#define VOLUME_BAR_TOP      (VOLUME_PLATE_Y + 76)
#define VOLUME_BAR_BOTTOM   (VOLUME_PLATE_Y + VOLUME_PLATE_H - 76)
#define VOLUME_ICON_CY      (VOLUME_PLATE_Y + VOLUME_PLATE_H - 40)

// "Loading..." and the failure reason, centred on the black screen
#define STATUS_TEXT_SIZE   24
#define STATUS_TEXT_WEIGHT UI_WEIGHT_BOLD

// subtitles: triangle cycles off -> track 1 -> track 2 -> ... -> off; the active cue draws low on the
// picture, above the bottom block while the HUD is up (and above the skip offer while that stands too), and
// the chip at the top right names the chosen track. The cue stays in FONT_POP (uiGetFallbackFont): the
// caption languages offered are the ones that font was checked to render.
#define SUBTITLE_TEXT_SIZE 30
#define SUBTITLE_MAX_W     1480   // wraps short of the volume plate on the left (and its mirror on the right)
#define SUBTITLE_BOTTOM    975    // the cue's bottom while the HUD is hidden
#define SUBTITLE_HUD_GAP   20     // clear of the HUD block (or the skip offer) above which it moves
#define SUBTITLE_PAD_X     16
#define SUBTITLE_PAD_Y     6
#define SUBTITLE_RADIUS    12

// the "Skip by choice" offer panel (Bild 2): the Triangle on a disc, "Skip <category>", and under it the
// category's colour and the time left in the segment
#define SKIP_PROMPT_TEXT    24
#define SKIP_PROMPT_WEIGHT  UI_WEIGHT_EXTRABOLD   // "Skip sponsor" in Bild 2: 24/800
#define SKIP_Y              760
#define SKIP_H              90
#define SKIP_PAD_L          16
#define SKIP_PAD_R          22
#define SKIP_RADIUS         UI_RADIUS_ROW         // 18
#define SKIP_RING_SCALE     0.9f                  // ring-r20-w3 at radius 18: Bild 2's inset ring, 2.7 px
#define SKIP_DISC_D         40
#define SKIP_GLYPH_H        30                    // the console's Triangle inside the 40-px disc
#define SKIP_TEXT_DX        (SKIP_PAD_L + SKIP_DISC_D + 16)   // 72: the panel's left -> its text
#define SKIP_TITLE_BASELINE 40                    // from the panel's top
#define SKIP_SUB_BASELINE   70
#define SKIP_SUB_SIZE       20
#define SKIP_SUB_WEIGHT     UI_WEIGHT_SEMIBOLD
#define SKIP_SUB_DOT        10
#define SKIP_SUB_DOT_GAP    8

// the stats (R3), description (SELECT) and chapter (START) panels: Ember panels in Manrope
#define PANEL_PAD_X        48
#define PANEL_HEADER_H     104    // the heading and that panel's own buttons, centred on y 52
#define PANEL_TITLE_SIZE   UI_SHELF_TITLE_SIZE
#define PANEL_TITLE_WEIGHT UI_SHELF_TITLE_WEIGHT
#define SCROLLBAR_W        6

// description overlay: SELECT toggles the description on a panel over the video; up/down scroll it. Body
// text (Ember 22-24/400) at the larger end, as a whole page of it is read from the sofa.
#define DESCRIPTION_PANEL_X     192
#define DESCRIPTION_PANEL_Y     96
#define DESCRIPTION_PANEL_W     (UI_SCREEN_W - 2 * DESCRIPTION_PANEL_X)                   // 1536
#define DESCRIPTION_PANEL_H     (UI_SCREEN_H - 2 * DESCRIPTION_PANEL_Y)                   // 888
#define DESCRIPTION_VIEW_X      (DESCRIPTION_PANEL_X + PANEL_PAD_X)
#define DESCRIPTION_VIEW_Y      (DESCRIPTION_PANEL_Y + PANEL_HEADER_H + 24)               // 224
#define DESCRIPTION_VIEW_H      (DESCRIPTION_PANEL_Y + DESCRIPTION_PANEL_H - 40 - DESCRIPTION_VIEW_Y)   // 720
#define DESCRIPTION_TEXT_W      (DESCRIPTION_PANEL_W - 2 * PANEL_PAD_X - 32)             // 1408, room for the bar
#define DESCRIPTION_FADE_H      32
#define DESCRIPTION_TEXT_SIZE   24
#define DESCRIPTION_TEXT_WEIGHT UI_BODY_WEIGHT
#define DESCRIPTION_SCROLL_STEP 40
#define DESCRIPTION_MAX_CUTS    8    // shortening steps for a text too long to render (renderDescription)
#define DESCRIPTION_CUT_REACH   48   // how far back from the resolve's cut the last whole word is looked for

// chapter picker: START toggles a centred list of the video's chapters on a panel; up/down select
// (the window follows the selection), cross jumps there, circle/START close without jumping.
#define CHAPTER_TEXT_SIZE    24
#define CHAPTER_TITLE_WEIGHT UI_WEIGHT_BOLD       // a row title (Ember 22-24/700); the time beside it 600
#define CHAPTER_TITLE_MAX_W  990
#define CHAPTER_ROW_HEIGHT   60
#define CHAPTER_VISIBLE_ROWS 12
#define CHAPTER_TIME_GAP     24   // between the right-aligned time column and the title column
#define CHAPTER_ROW_INSET    24   // the selected row's plate reaches this far past the text on either side
#define CHAPTER_LIST_PAD_TOP 12
#define CHAPTER_LIST_PAD_BOTTOM 28
#define CHAPTER_PANEL_MIN_W  840  // the heading and the panel's own buttons need this much
#define CHAPTER_PANEL_MAX_W  1536

// the HUD's hint row, in Bild 2's order plus Back (UEBERGABE 6); the indices are uiAddHint's, in this order
#define HINT_CROSS    0
#define HINT_L1       1
#define HINT_R1       2
#define HINT_L2       3
#define HINT_R2       4
#define HINT_SQUARE   5
#define HINT_TRIANGLE 6
#define HINT_START    7
#define HINT_SELECT   8
#define HINT_CIRCLE   9

// where the SponsorBlock legend sits this frame (layoutLegend)
typedef enum { LEGEND_NONE, LEGEND_IN_HINT_ROW, LEGEND_IN_ROW_A, LEGEND_OWN_LINE } LegendPlace;

// the video to play, set by playVideo() before the screen is pushed. defaults to
// a short public clip so pushing the screen directly still does something.
static char requestedInput[256] = "jNQXAC9IVRw";   // "Me at the zoo"

typedef enum { STAGE_LOADING, STAGE_FAILED, STAGE_PLAYING } Stage;

static struct {
   volatile Stage   stage;
   char             message[128];     // failure reason (worker writes, UI reads)
   VideoPlayer     *pendingPlayer;    // built by the worker, adopted by the UI thread
   volatile int     workerDone;
   int              threadActive;
   sys_ppu_thread_t workerTid;
   VideoPlayer     *player;
   int              screenW, screenH;
   int              firstFrameSeen;   // a real frame has been presented (loading status ends, save is safe)
   int              showStats;        // R3 toggles a debug overlay
   char             title[256];        // resolved video title (worker sets, UI reads after adoption)
   char             description[2048]; // resolved video description (worker sets, UI reads after adoption)
   int              descriptionCut;    // the resolve cut it to fit (it filled StreamInfo.description)

   // description overlay: the text is rasterised once, on first open, into one tall texture that
   // up/down scroll. playback carries on untouched underneath.
   int              descriptionVisible;
   int              descriptionRendered;
   int              descriptionScroll;   // pixels scrolled down into the text

   // chapter picker: chapters come out of the description at resolve time; when that finds none, a
   // side worker asks the /next endpoint (YouTube's own chapter list). the row labels are rasterised
   // on first open, and the widest row sizes the panel.
   ChapterList      chapters;            // parsed by the worker (UI reads after adoption)
   ChapterList      fetchedChapters;     // /next result (worker writes, copied over on reap)
   volatile int     chWorkerDone;
   int              chThreadActive, chFetchTried;
   sys_ppu_thread_t chWorkerTid;
   int              chaptersVisible;
   int              chaptersRendered;
   int              chapterSelected;
   int              chapterTimeWidth;    // widest time label (the right-aligned time column)
   int              chapterPanelWidth;
   int              vidItag, vidW, vidH, vidFps, audItag;   // picked formats (worker sets, UI reads)
   int              isLive;           // a live stream: no duration, no resume/save, no seek

   // audio tracks (Square): a video YouTube serves in several languages - the uploader's own, or its AI
   // auto-dubs - keeps every track's url from the resolve, so Square can reopen the player on another one at
   // the current position without resolving again. The extractor sorts the original first, and that is the
   // track that starts playing.
   AudioTrack       audioTracks[MAX_AUDIO_TRACKS];   // copied from the resolve (worker sets)
   int              audioTrackCount;   // 0 unless there are at least two to choose between
   int              audioTrackPick;    // index of the track playing now
   char             videoUrl[MAX_STREAM_URL];   // the video stream (every track pairs with it); kept for a reopen too
   char             audioUrl[MAX_STREAM_URL];   // the separate audio stream it opened with ("" = muxed)
   int              reopenPending;     // the player is being rebuilt after the end (reopenAt)
   float            reopenAtSec;       // where the rebuilt player starts
   int              switchTarget;      // switchWorker input: the track it is building
   int              switchPending;     // an audio switch is building (the old player is already freed)
   int              switchReverting;   // this build is the fallback to the track we came from
   int              switchWasPaused;   // carry the paused state across the rebuild
   float            switchStartSec;    // position to resume the rebuilt player at
   char             audioNotice[96];   // "Audio 2/5: German (auto-dubbed)", shown once the picture is back
   int              audioNoticePending;
   const uint8_t   *lastFrame;        // fps measure: a changed pointer means a newly presented frame
   int              framesThisSecond, measuredFps;
   uint64_t         fpsTickUs;

   // seek: the UI owns the scrub target so the bar moves instantly; the engine seek is deferred until
   // the input goes quiet (each apply costs a decoder flush + fragment reload)
   int              seeking;
   float            seekTarget;
   float            seekFrom;          // the playhead when this scrub began: a target past it is a step forward
   uint64_t         lastSeekInputUs;
   uint64_t         controlsShownUs;   // last activity, for the bar/time auto-hide

   // sponsorblock: fetched on a side thread (in parallel with resolve/open), then auto-skipped during
   // playback and drawn on the seek bar. sbReady goes up once the thread is reaped and segments are safe.
   SponsorSegments  sb;
   volatile int     sbWorkerDone;
   int              sbThreadActive, sbReady;
   int              skipPrompt;        // index of the segment offering a manual skip, -1 when none
   int              skipPromptShown;   // which segment the prompt label was last rasterised for
   sys_ppu_thread_t sbWorkerTid;
   uint64_t         toastShownUs;      // brief toast timer (0 = hidden)

   // subtitles: triangle picks a track (-1 = off, the default); a worker fetches it in the background
   // (updatePlay respawns the fetch whenever the pick and the fetched track disagree), and once ready
   // the cue for the current position drives subtitleLabel each frame.
   CaptionTrack     captionTracks[MAX_CAPTION_TRACKS];   // copied from the resolve (worker sets)
   int              captionCount;
   int              subtitlePick;        // chosen track index, -1 = off
   int              subtitleFetched;     // track index the fetch (in flight or done) belongs to, -1 = none
   int              subtitlesReady;      // fetched track is safe to read
   int              subtitleScanFrom;    // cue lookup cache (getSubtitleCueText)
   SubtitleTrack   *subtitleTrack;       // heap; ~800 KB, allocated on first enable, freed on exit
   volatile int     subWorkerDone;
   int              subThreadActive;
   sys_ppu_thread_t subWorkerTid;

   // the HUD (Bild 2): what its labels were last set for, so they rasterise only when that changes
   int              hudChapter;          // chapter row A names, -1 = none (no chapters, or live)
   int              hudChapterCount;     // ...out of how many
   int              skipTimeReserveW;    // the offer's countdown at its widest (the segment's length)
   int              crossCaptionMaxW;    // the widest the Cross / Triangle captions get: the legend's
   int              triangleCaptionMaxW; // place is judged by these, so it does not hop on pause or offer
   int              chapterReserveFor;   // chapter count the row-A reserve below was measured for
   int              chapterReserveW;     // the widest chapter block row A can show for this chapter list

   // the SponsorBlock legend: the categories that occur in this video, in the order they first come along
   // the timeline, and where it fits this frame
   int              legendBuilt;
   int              legendCount;
   int              legendWidth;
   SponsorCategory  legendCategories[SPONSOR_CATEGORY_COUNT];
   LegendPlace      legendPlace;
} state;

#define STAT_LINES 4
// R3's stats panel: in body text, on a panel under the title block and clear of the volume plate below it
#define STATS_TEXT_SIZE    UI_BODY_SIZE
#define STATS_TEXT_WEIGHT  UI_BODY_WEIGHT
#define STATS_Y            (TITLE_PLATE_BOTTOM + CHIP_GAP)   // 201
#define STATS_PANEL_W      500
#define STATS_PAD_X        24
#define STATS_PAD_Y        20
#define STATS_LINE_H       32

// The fonts are the app's shared ones (ui-fonts.h), opened once in main.c: none is opened or closed here.
static Label statusLabel;
static Label statLabels[STAT_LINES];
static UiDigits timeDigits;                   // row A's position (24/700) and the volume percentage
static UiDigits totalDigits;                  // row A's " / total" (24/600) and the chapter list's start times
static UiDigits skipDigits;                   // the skip offer's countdown (20/600)
static Label toastLabel;                      // brief toast ("Skipped ...", "Autoplay on", ...)
static Label skipPromptLabel;                 // "Skip Intro" on the offer panel
static Label skipSuffixLabel;                 // " · SponsorBlock" after the offer's countdown
static Label titleLabel;                      // video title, top-left while the HUD is up
static Label channelLabel;                    // the list entry's channel, above the title
static Label metaDotLabel;                    // the dot between channel and views
static Label viewsLabel;                      // the list entry's view count
static Label qualityLabel;                    // quality chip ("1080p")
static Label audioNameLabel;                  // audio chip: the track playing...
static Label audioCountLabel;                 // ...and " · n/m"
static Label subtitleLabel;                   // active subtitle cue
static Label subsHudLabel;                    // subtitle chip: "Subtitles off" / "Subtitles: <track>"
static Label chapterNameLabel;                // row A: the chapter the playhead is in
static Label chapterInfoLabel;                // row A: " · Chapter n of m"
static Label liveLabel;                       // row A on a live stream, where the time would be
static Label legendLabels[SPONSOR_CATEGORY_COUNT];   // the legend's category names
static Label chapterLabels[MAX_CHAPTERS];     // chapter titles (picker overlay rows)
static Label descriptionHeading, chaptersHeading;    // the two overlay panels' headings
static UiHints hudHints;                      // the HUD's hint row
static UiHints descriptionHints, chapterHints;       // each overlay's own buttons, in its heading
static TextTexture descriptionTexture;        // whole description as one tall texture (scrolled by draw)

// the library meter (input, repeat, level; drawn by drawVolume); its level is kept in `volumeLevel` outside
// `state` so it survives across videos.
static VolumeMeter volumeMeter;
static int volumeLevel = VOLUME_DEFAULT;

static void fail(const char *reason)
{
   logError("[yt] play failed: %s\n", reason);
   strCopy(state.message, sizeof state.message, reason);
   __sync_synchronize();   // publish message before the UI thread sees STAGE_FAILED
   state.stage = STAGE_FAILED;
}

// only languages FONT_POP renders reliably (hardware-checked: western/central europe + japanese fine,
// vietnamese diacritics broken). prefix match, so "en-GB" passes as "en". extend after testing a new
// language on the console.
static int isRenderableSubtitleLanguage(const char *code)
{
   static const char *supported[] = { "en", "es", "pt", "fr", "de", "it", "nl", "sv", "da", "no", "fi",
                                      "pl", "cs", "sk", "hu", "ro", "tr", "id", "ms", "ja" };
   for (int i = 0; i < (int)(sizeof supported / sizeof *supported); i++)
      if (strncmp(code, supported[i], strlen(supported[i])) == 0) return 1;
   return 0;
}

static void worker(uint64_t arg)
{
   (void)arg;
   StreamInfo *info = malloc(sizeof *info);   // ~140 KB, too big for the stack
   const Extractor *extractor = findExtractor(requestedInput);
   if (!info || !extractor) { fail("unrecognised link or id"); goto done; }

   // resolve (stage stays STAGE_LOADING, set by initPlay's memset, until we adopt or fail)
   if (extractor->extract(requestedInput, info) != 0 || info->formatCount == 0) { fail("resolve failed"); goto done; }
   strCopy(state.title, sizeof state.title, info->title);
   strCopy(state.description, sizeof state.description, info->description);
   // the resolve keeps the first 2047 bytes of a description, so one that fills them was cut there (one of
   // exactly that length reads as cut too, which costs it no more than a "..."): renderDescription says so
   state.descriptionCut = (int)strlen(info->description) >= (int)sizeof info->description - 1;
   parseChapters(state.description, &state.chapters);
   for (int i = 0; i < info->captionCount; i++)   // keep only tracks the system font can render
      if (isRenderableSubtitleLanguage(info->captions[i].languageCode))
         state.captionTracks[state.captionCount++] = info->captions[i];

   const StreamFormat *video = pickBestVideo(info);
   if (!video) { fail("no playable mp4 video"); goto done; }
   // a muxed pick (itag 18) already carries audio; a video-only pick needs a separate audio track
   const StreamFormat *audio = video->hasAudio ? NULL : pickBestAudio(info);
   logInfo("[yt] play video itag %d %dx%d %s, audio itag %d\n", video->itag, video->width, video->height,
           video->hasAudio ? "muxed" : "video-only", audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   state.vidItag = video->itag; state.vidW = video->width; state.vidH = video->height; state.vidFps = video->fps;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);
   state.isLive = video->isLiveSegmented;

   // several audio tracks: keep them for Square. Only behind a video-only stream - a muxed one (itag 18)
   // carries its own sound - and not live, whose rolling segment window a rebuild would have to rejoin.
   if (audio && !video->hasAudio && !state.isLive && info->audioTrackCount > 1) {
      int playing = -1;
      for (int i = 0; i < info->audioTrackCount; i++)
         if (strcmp(info->audioTracks[i].url, audio->url) == 0) { playing = i; break; }
      if (playing >= 0) {
         memcpy(state.audioTracks, info->audioTracks, info->audioTrackCount * sizeof *info->audioTracks);
         strCopy(state.videoUrl, sizeof state.videoUrl, video->url);
         state.audioTrackPick = playing;
         state.audioTrackCount = info->audioTrackCount;
         logInfo("[yt] %d audio tracks, playing %s\n", state.audioTrackCount, state.audioTracks[playing].name);
      }
   }

   // the streams, kept: a video played to its end is reopened from them (reopenAt), not resolved again
   strCopy(state.videoUrl, sizeof state.videoUrl, video->url);
   strCopy(state.audioUrl, sizeof state.audioUrl, audio ? audio->url : "");

   // open the decoder: both streams ride the http module by range request. an audio open
   // failure inside the player just means silent playback. NULL if it can't be demuxed/decoded.
   state.pendingPlayer = createVideoPlayerSplit(video->url, audio ? audio->url : NULL, allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (!state.pendingPlayer) { fail("couldn't open stream"); goto done; }

   // resume where we left off. seekVideoPlayer only posts the target; the decode thread does the
   // reconnect + fragment reload. drawPlay keeps the "Loading..." status up until the first frame is
   // presented, so that reload is covered rather than showing a black screen. storage returns 0 for a
   // typed url (never a history key), so this no-ops for anything but a bare videoId.
   {
      int resumeAt = state.isLive ? 0 : getWatchedPosition(requestedInput);
      float duration = getVideoDurationSeconds(state.pendingPlayer);
      if (resumeAt > 3 && (duration <= 0 || resumeAt < duration - 10)) seekVideoPlayer(state.pendingPlayer, (float)resumeAt);
   }

done:
   free(info);
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

// a bare 11-char videoId (not a typed url) is what SponsorBlock and history key on; typed urls no-op.
static int isBareVideoId(const char *input)
{
   return input[0] && !strchr(input, '/') && !strchr(input, ':');
}

// fetch skip segments in parallel with resolve/open, so they cost no startup latency.
static void sponsorWorker(uint64_t arg)
{
   (void)arg;
   fetchSponsorSegments(requestedInput, &state.sb);
   __sync_synchronize();
   state.sbWorkerDone = 1;
   exitThread();
}

// asks /next for youtube's own chapter list; spawned only when the description parse found none.
static void chapterWorker(uint64_t arg)
{
   (void)arg;
   fetchChapters(requestedInput, &state.fetchedChapters);
   __sync_synchronize();
   state.chWorkerDone = 1;
   exitThread();
}

// ---- autoplay queue ----
//
// The whole entries are kept, not just the ids. Ids alone were cheaper (~8 kB against ~160 kB), but they
// left an autoplayed video with no title, channel or duration to record - so it could be marked watched but
// never entered in the history, and "Continue watching" could not list the one thing it exists for: the
// video you walked away from. 160 kB is nothing next to the category caches this app already holds.
// playQueueIndex is where the video now playing sits in that list.
static SearchResult playQueue[MAX_SEARCH_RESULTS];
static int  playQueueCount, playQueueIndex;

static void beginPlayback(const char *input)
{
   strCopy(requestedInput, sizeof requestedInput, input);
   pushScreen(&playScreen);
}

void playVideo(const char *input)
{
   playQueueCount = playQueueIndex = 0;   // a one-off play has nothing queued behind it
   beginPlayback(input);
}

void playVideoFromList(const SearchResults *results, int index)
{
   if (!results || index < 0 || index >= results->count) return;
   playQueueCount = 0;
   for (int i = 0; i < results->count && playQueueCount < MAX_SEARCH_RESULTS; i++)
      playQueue[playQueueCount++] = results->items[i];
   playQueueIndex = index;
   beginPlayback(results->items[index].videoId);
}

// A finished video hands over to the next entry by restarting the screen rather than reloading in place:
// the ordinary teardown already saves the resume position, joins four workers and destroys the player, and
// duplicating that as a second path is exactly how one of them ends up forgotten. 1 if it took over.
static int advanceAutoplay(void)
{
   if (!getAutoplay() || state.isLive) return 0;
   if (playQueueIndex + 1 >= playQueueCount) return 0;

   const SearchResult *next = &playQueue[++playQueueIndex];
   markWatchedItem(next);   // the full entry, so History and "Continue watching" can list it later
   popScreen();             // terminates this screen (saving the finished video's position)
   beginPlayback(next->videoId);   // ...and starts the next one; the queue lives outside the screen state
   return 1;
}

// the width a hint caption will have, without rendering it (the captions are Manrope, ui-hints.c)
static int measureHintCaption(const char *text)
{
   return (int)(measureFontText(uiGetFont(UI_HINT_WEIGHT), UI_HINT_SIZE, text) + 0.999f);
}

static int maxInt(int a, int b) { return a > b ? a : b; }

// The HUD's hint row (Bild 2's order, plus Back), and each overlay's own buttons for its heading. The
// captions that change with the state start as one of their forms; updateHintCaptions keeps them true.
static void initHints(void)
{
   uiInitHints(&hudHints);
   uiAddHint(&hudHints, GLYPH_CROSS,    "Pause");         // HINT_CROSS: Play while paused, Replay once ended
   uiAddHint(&hudHints, GLYPH_L1,       "");              // HINT_L1, paired with
   uiAddHint(&hudHints, GLYPH_R1,       "Chapter");       // HINT_R1
   uiAddHint(&hudHints, GLYPH_L2,       "");              // HINT_L2, paired with
   uiAddHint(&hudHints, GLYPH_R2,       "\xC2\xB1" "1 min");   // HINT_R2: "±1 min"
   uiAddHint(&hudHints, GLYPH_SQUARE,   "Audio track");   // HINT_SQUARE: Autoplay with a single track
   uiAddHint(&hudHints, GLYPH_TRIANGLE, "Subtitles");     // HINT_TRIANGLE: Skip while the offer stands (on a
                                                          // live stream it shows only then)
   uiAddHint(&hudHints, GLYPH_START,    "Chapters");      // HINT_START
   uiAddHint(&hudHints, GLYPH_SELECT,   "Description");   // HINT_SELECT
   uiAddHint(&hudHints, GLYPH_CIRCLE,   "Back");          // HINT_CIRCLE
   uiSetHintsGap(&hudHints, HINT_ROW_GAP);
   state.crossCaptionMaxW    = maxInt(measureHintCaption("Pause"), maxInt(measureHintCaption("Play"),
                                                                          measureHintCaption("Replay")));
   state.triangleCaptionMaxW = maxInt(measureHintCaption("Subtitles"), measureHintCaption("Skip"));

   // the overlays own the pad while they are up (handleDescriptionInput / handleChaptersInput)
   uiInitHints(&descriptionHints);
   uiAddHint(&descriptionHints, GLYPH_SELECT,    "");
   uiAddHint(&descriptionHints, GLYPH_CIRCLE,    "Close");
   uiAddHint(&descriptionHints, GLYPH_DPAD_UP,   "");
   uiAddHint(&descriptionHints, GLYPH_DPAD_DOWN, "Scroll");

   uiInitHints(&chapterHints);
   uiAddHint(&chapterHints, GLYPH_CROSS,     "Jump");
   uiAddHint(&chapterHints, GLYPH_CIRCLE,    "");
   uiAddHint(&chapterHints, GLYPH_START,     "Close");
   uiAddHint(&chapterHints, GLYPH_DPAD_UP,   "");
   uiAddHint(&chapterHints, GLYPH_DPAD_DOWN, "Move");
}

static void initPlay(void)
{
   memset(&state, 0, sizeof state);
   state.skipPrompt = state.skipPromptShown = -1;   // 0 is a real segment index; "none" has to be -1
   state.hudChapter = -1;
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();

   // fixed text - the app's own English and numbers - is Manrope
   uiInitLabel(&statusLabel, STATUS_TEXT_WEIGHT, 0, 0, 1400, STATUS_TEXT_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP_ELLIPSIS, "");
   for (int i = 0; i < STAT_LINES; i++)
      uiInitLabel(&statLabels[i], STATS_TEXT_WEIGHT, 0, 0, STATS_PANEL_W - 2 * STATS_PAD_X, STATS_TEXT_SIZE,
                  activeTheme->textPrimary, TEXT_NOWRAP, "");
   uiInitDigits(&timeDigits, TIME_TEXT_WEIGHT, TIME_TEXT_SIZE);
   uiInitDigits(&totalDigits, TOTAL_TEXT_WEIGHT, TIME_TEXT_SIZE);
   uiInitDigits(&skipDigits, SKIP_SUB_WEIGHT, SKIP_SUB_SIZE);
   uiInitLabel(&skipPromptLabel, SKIP_PROMPT_WEIGHT, 0, 0, AUTO, SKIP_PROMPT_TEXT, activeTheme->textPrimary, TEXT_NOWRAP, "");
   uiInitLabel(&skipSuffixLabel, SKIP_SUB_WEIGHT, 0, 0, AUTO, SKIP_SUB_SIZE, activeTheme->textSecondary, TEXT_NOWRAP,
               " \xC2\xB7 SponsorBlock");
   uiInitLabel(&metaDotLabel, META_VIEWS_WEIGHT, 0, 0, AUTO, META_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "\xC2\xB7");
   uiInitLabel(&qualityLabel, CHIP_TEXT_WEIGHT, 0, 0, AUTO, CHIP_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "");
   uiInitLabel(&audioCountLabel, CHIP_TEXT_WEIGHT, 0, 0, AUTO, CHIP_TEXT_SIZE, activeTheme->accentText, TEXT_NOWRAP, "");
   uiInitLabel(&chapterInfoLabel, CHAPTER_INFO_WEIGHT, 0, 0, AUTO, ROW_A_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   uiInitLabel(&liveLabel, UI_BADGE_WEIGHT, 0, 0, AUTO, UI_BADGE_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "LIVE");
   uiInitLabel(&descriptionHeading, PANEL_TITLE_WEIGHT, 0, 0, AUTO, PANEL_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Description");
   uiInitLabel(&chaptersHeading, PANEL_TITLE_WEIGHT, 0, 0, AUTO, PANEL_TITLE_SIZE, activeTheme->textPrimary,
               TEXT_NOWRAP, "Chapters");
   initHints();
   // text from YouTube - the title, the channel and views, track names (in the toasts and the chips), chapter
   // titles - is raw, and the whole of it is FONT_POP whenever Manrope lacks one of its characters
   // (uiSetTextLabel)
   uiInitTextLabel(&toastLabel, TOAST_TEXT_WEIGHT, 0, 0, TOAST_MAX_WIDTH, TOAST_TEXT_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, "");
   uiInitTextLabel(&titleLabel, TITLE_WEIGHT, 0, 0, TITLE_MAX_W, TITLE_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, "");
   uiInitTextLabel(&subsHudLabel, CHIP_TEXT_WEIGHT, 0, 0, SUBS_CHIP_MAX_W, CHIP_TEXT_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, "");
   uiInitTextLabel(&audioNameLabel, CHIP_TEXT_WEIGHT, 0, 0, AUDIO_NAME_MAX_W, CHIP_TEXT_SIZE, activeTheme->accentText,
                   TEXT_NOWRAP_ELLIPSIS, "");
   uiInitTextLabel(&chapterNameLabel, CHAPTER_NAME_WEIGHT, 0, 0, CHAPTER_NAME_MAX_W, ROW_A_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, "");
   // channel and views are known only for a video started from a list: the queue entry it came from
   {
      const SearchResult *entry = playQueueCount > 0 ? &playQueue[playQueueIndex] : NULL;
      uiInitTextLabel(&channelLabel, META_CHANNEL_WEIGHT, 0, 0, CHANNEL_MAX_W, META_SIZE, activeTheme->textPrimary,
                      TEXT_NOWRAP_ELLIPSIS, entry ? entry->author : "");
      uiInitTextLabel(&viewsLabel, META_VIEWS_WEIGHT, 0, 0, VIEWS_MAX_W, META_SIZE, activeTheme->textSecondary,
                      TEXT_NOWRAP_ELLIPSIS, entry ? entry->views : "");
   }
   for (int i = 0; i < MAX_CHAPTERS; i++)
      uiInitTextLabel(&chapterLabels[i], CHAPTER_TITLE_WEIGHT, 0, 0, CHAPTER_TITLE_MAX_W, CHAPTER_TEXT_SIZE,
                      activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&subtitleLabel, uiGetFallbackFont(), 0, 0, SUBTITLE_MAX_W, AUTO, SUBTITLE_TEXT_SIZE,
                activeTheme->textPrimary, TEXT_WRAP, "");
   state.subtitlePick = state.subtitleFetched = -1;   // default off (memset left them 0)

   initVolumeMeter(&volumeMeter, uiGetFont(VOLUME_NUMBER_WEIGHT), activeTheme->accent, volumeLevel);
   layoutVolumeMeter(&volumeMeter, state.screenW, state.screenH);

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail("couldn't start worker");

   // fetch whenever ANY category is set to skip or to ask - an "ask" category needs the segments just as
   // much as an automatic one, and forgetting the second mask here would have made the new option silently
   // do nothing on a video whose other categories were all set to Keep
   if ((getSkipCategories() | getAskCategories()) != 0 && isBareVideoId(requestedInput))
      state.sbThreadActive = (spawnJoinableThread(&state.sbWorkerTid, sponsorWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-sponsor") == 0);
}

static void showControls(void) { state.controlsShownUs = sys_time_get_system_time(); }

static void formatTime(char *buffer, int cap, int seconds)
{
   if (seconds < 0) seconds = 0;
   if (seconds >= 3600) snprintf(buffer, cap, "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
   else                 snprintf(buffer, cap, "%d:%02d", seconds / 60, seconds % 60);
}

static void showToast(const char *text)
{
   uiSetTextLabel(&toastLabel, TOAST_TEXT_WEIGHT, text);   // may carry a track name or a chapter title
   state.toastShownUs = sys_time_get_system_time();
   showControls();
}

// fetches the picked caption track in the background; updatePlay reaps it and respawns on a new pick.
static void subtitleWorker(uint64_t arg)
{
   (void)arg;
   fetchSubtitles(state.captionTracks[state.subtitleFetched].url, state.subtitleTrack);   // a failure leaves count 0
   __sync_synchronize();
   state.subWorkerDone = 1;
   exitThread();
}

// triangle: cycle off -> track 1 -> ... -> off; the pick shows in the subtitle chip at the top right. the
// fetch itself is driven by updatePlay, so a mid-fetch switch just supersedes the old pick.
static void cycleSubtitles(void)
{
   // live captions use a rolling delivery our fetcher can't parse, so live streams count as none
   if (state.captionCount == 0 || state.isLive) { showToast("No subtitles"); return; }
   state.subtitlePick = state.subtitlePick + 1 < state.captionCount ? state.subtitlePick + 1 : -1;

   setLabelText(&subtitleLabel, "");   // clear the old track's cue; the new one repopulates once fetched

   char hud[64];
   if (state.subtitlePick < 0) strCopy(hud, sizeof hud, "Subtitles off");
   else snprintf(hud, sizeof hud, "Subtitles: %s", state.captionTracks[state.subtitlePick].name);
   uiSetTextLabel(&subsHudLabel, CHIP_TEXT_WEIGHT, hud);
   showControls();
}

// build the player for the switchTarget audio track at switchStartSec. Runs on a worker thread because
// createVideoPlayerSplit blocks on network i/o; the old player is already gone (cycleAudioTrack frees it
// before spawning this), so two sets of video frame buffers never have to fit at once.
static void switchWorker(uint64_t arg)
{
   (void)arg;
   VideoPlayer *player = createVideoPlayerSplit(state.videoUrl, state.audioTracks[state.switchTarget].url,
                                                allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (player && state.switchStartSec > 1.0f) seekVideoPlayer(player, state.switchStartSec);
   state.pendingPlayer = player;
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

static void startSwitchWorker(void)
{
   state.workerDone = 0;
   state.threadActive = (spawnJoinableThread(&state.workerTid, switchWorker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-audio") == 0);
   if (!state.threadActive) { state.switchPending = 0; fail("couldn't switch audio track"); }
}

// Square, on a video with several audio tracks: the next track, from where the video is now. The player
// cannot swap its audio stream in place, so this is mohasi's resolution switch (upstream, b263bf9) applied to
// audio: the current player is torn down, a worker builds the new one and seeks it to the same position,
// and the screen says "Loading..." meanwhile. updatePlay adopts it - or, when the track will not open,
// rebuilds the one we came from, so a bad track costs a second's reload rather than the video.
static void cycleAudioTrack(void)
{
   if (state.stage != STAGE_PLAYING || !state.player || state.threadActive) return;   // still opening one

   state.switchTarget = (state.audioTrackPick + 1) % state.audioTrackCount;
   state.switchPending = 1;
   state.switchReverting = 0;
   state.switchWasPaused = isVideoPaused(state.player);
   state.switchStartSec = state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
   state.seeking = 0;   // the rebuilt player starts at the scrub target; there is nothing left to apply

   // save the position now, under termPlay's rule (a frame has played, not live): leaving during the
   // rebuild, or a rebuild that fails, would otherwise lose it - termPlay only saves with a player up
   if (state.firstFrameSeen && !state.isLive) setWatchedPosition(requestedInput, (int)state.switchStartSec);

   finishGfx();   // the RSX may still be sampling the current frame buffer
   destroyVideoPlayer(state.player);
   state.player = NULL;
   state.firstFrameSeen = 0;
   state.stage = STAGE_LOADING;
   startSwitchWorker();
}

// Rebuilds the player from the streams it was opened with and starts it at reopenAtSec - the same rebuild an
// audio-track switch does, on whatever track is playing. Runs on a worker: createVideoPlayerSplit blocks on the
// network.
static void reopenWorker(uint64_t arg)
{
   (void)arg;
   const char *audio = state.audioTrackCount > 1 ? state.audioTracks[state.audioTrackPick].url
                                                 : (state.audioUrl[0] ? state.audioUrl : NULL);
   VideoPlayer *player = createVideoPlayerSplit(state.videoUrl, audio, allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (player && state.reopenAtSec > 1.0f) seekVideoPlayer(player, state.reopenAtSec);
   state.pendingPlayer = player;
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

// A video that has played to its end has spent its streams: a seek on them only starves - "reader starved"
// every 3 s and the HUD frozen on 0:00, found in RPCS3 on X (Replay) at the end, after which O froze the app.
// So once the video has ended, Replay and every later seek rebuild the player instead, at the target, and the
// screen shows "Loading..." meanwhile, as for an audio switch.
static void reopenAt(float seconds)
{
   if (state.threadActive || !state.videoUrl[0]) return;   // a worker is still busy; or nothing to reopen from
   state.reopenAtSec = seconds;
   state.reopenPending = 1;
   state.seeking = 0;   // the rebuilt player starts at the target; there is nothing left to apply

   finishGfx();   // the RSX may still be sampling the current frame buffer
   destroyVideoPlayer(state.player);
   state.player = NULL;
   state.firstFrameSeen = 0;
   state.stage = STAGE_LOADING;
   state.workerDone = 0;
   state.threadActive = (spawnJoinableThread(&state.workerTid, reopenWorker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-reopen") == 0);
   if (!state.threadActive) { state.reopenPending = 0; fail("couldn't reopen the video"); }
}

// Every seek the viewer asks for goes through here. atOrAfter lands on the first keyframe at or after the target
// (forward steps, chapter starts), otherwise on the last one before it. After the end it is a reopen (above).
static void seekTo(float seconds, int atOrAfter)
{
   if (!state.player) return;
   if (isVideoEnded(state.player) && !state.isLive) { reopenAt(seconds); return; }
   if (atOrAfter) seekVideoPlayerPast(state.player, seconds);
   else           seekVideoPlayer(state.player, seconds);
}

// accumulates a scrub target while left/right is held; the engine seek fires once input goes quiet
static void nudgeSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.seeking) {
      state.seeking = 1;
      state.seekFrom = state.seekTarget = getVideoPositionSeconds(state.player);
   }
   state.seekTarget += deltaSeconds;
   float duration = getVideoDurationSeconds(state.player);
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (duration > 0.0f && state.seekTarget > duration) state.seekTarget = duration;
   state.lastSeekInputUs = nowUs;
   showControls();
}

// the position the HUD shows and L1/R1 step from: the scrub target while scrubbing, so the time, the chapter row
// and the knob never disagree during left/right or L2/R2
static float hudPosition(void)
{
   return state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
}

// the chapter at a position: the last one starting at or before it
static int chapterAt(float position)
{
   int current = 0;
   for (int i = 0; i < state.chapters.count; i++)
      if (state.chapters.chapters[i].start <= position) current = i;
   return current;
}

// left/right scrub, cross toggles pause (restarts if the video ended). only once playing.
// L1/R1 step chapter to chapter without opening the picker. The jump is immediate (no scrub deferral), and a
// toast names the chapter it went to.
static void jumpChapter(int direction)
{
   if (state.chapters.count == 0) { showToast("No chapters"); return; }

   float position = hudPosition();
   int current = chapterAt(position);

   int target = current + direction;
   // stepping back from the middle of a chapter restarts it rather than skipping to the previous one
   if (direction < 0 && position - state.chapters.chapters[current].start > CHAPTER_REWIND_GRACE) target = current;
   if (target < 0) target = 0;
   if (target >= state.chapters.count) target = state.chapters.count - 1;

   state.seeking = 0;   // a pending scrub must not overwrite the jump
   // on the first keyframe at or after the chapter's start: the one before it lies in the previous chapter, so
   // R1 used to land up to a few seconds early, with the previous chapter's name on screen
   seekTo(state.chapters.chapters[target].start, 1);
   showControls();

   char notice[96];
   snprintf(notice, sizeof notice, "%d/%d  %s", target + 1, state.chapters.count, state.chapters.chapters[target].title);
   showToast(notice);
}

// defined below with the rest of the sponsor handling; used here because Triangle is answered in this
// function while the offer panel is up
static void skipSegment(SponsorSegment *segment);

static void handlePlaybackInput(void)
{
   uint64_t nowUs = sys_time_get_system_time();

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      showControls();
      // Replay rebuilds the player (seekTo): there is no player left for the rest of this frame's input
      if (isVideoEnded(state.player)) { seekTo(0.0f, 0); return; }
      setVideoPaused(state.player, !isVideoPaused(state.player));
   }

   // Triangle belongs to the skip offer while that panel is up (see findSkipOffer); subtitles get it back
   // the moment the segment is behind us. Suspending one rarely-used binding for a few seconds is the price
   // of a contextual button, and the panel says on screen what the button does right now.
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      if (state.skipPrompt >= 0) { skipSegment(&state.sb.segments[state.skipPrompt]); state.skipPrompt = -1; }
      else                       cycleSubtitles();
   }

   // volume: the widget steps itself on up/down; apply + persist the new level (works on live streams too)
   if (handleVolumeMeterInput(&volumeMeter)) {
      volumeLevel = volumeMeter.level;
      setAudioPcmFeedVolume(getVolumeMeterFraction(&volumeMeter));
   }

   if (state.isLive) return;   // a live stream has no seek index; scrubbing would break the segment stream

   if      (isPadButtonPressed(PAD_BTN_R1)) { jumpChapter(1);  return; }
   else if (isPadButtonPressed(PAD_BTN_L1)) { jumpChapter(-1); return; }

   static ButtonRepeat seekRepeat, fastSeekRepeat;
   if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_RIGHT)))     nudgeSeek(+SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_LEFT))) nudgeSeek(-SEEK_STEP_SECONDS, nowUs);
   if      (isRepeatDue(&fastSeekRepeat, getPadButtonState(PAD_BTN_R2))) nudgeSeek(+FAST_SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&fastSeekRepeat, getPadButtonState(PAD_BTN_L2))) nudgeSeek(-FAST_SEEK_STEP_SECONDS, nowUs);

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      // A step forward lands on the first keyframe at or after the target, a step back on the one before it.
      // Rounding forward steps down as well left RIGHT (+5 s) stuck wherever a fragment ran longer than the
      // step: every press landed on the keyframe the playhead had just come from, some a second behind it
      // (seen on a 1080p30 video whose fragment ran from 174 s past 180 s). The DASH streams the player
      // normally gets honour this; a plain mp4 or mkv still lands at or before the target.
      seekTo(state.seekTarget, state.seekTarget > state.seekFrom);
      state.seeking = 0;
   }
}

// which categories are auto-skipped, straight from the settings screen - one bit per category, so "skip the
// intro but keep the outro" is a thing the user can actually say. (Every fetched segment is still marked on
// the seek bar whatever is switched on, so nothing is hidden, only jumped.)
static int shouldAutoSkipSponsor(SponsorCategory category)
{
   return getSponsorAction(category) == SPONSOR_ACTION_SKIP;
}

// Jump past a segment and say so - shared by the automatic skip and the manual one, so the two can never
// drift apart on the details that matter (the one-shot guard, and landing on a keyframe AFTER the segment).
static void skipSegment(SponsorSegment *segment)
{
   segment->skipped = 1;
   seekVideoPlayerPast(state.player, segment->end);
   char notice[64];
   snprintf(notice, sizeof notice, "Skipped %s", getSponsorCategoryName(segment->category));
   showToast(notice);
}

// The offer for a category set to "Skip by choice": while the playhead is inside such a segment, a panel
// appears and Triangle takes it. Returns the segment index, or -1.
//
// Triangle normally cycles subtitles; while the panel is up that is suspended. Be honest about the cost:
// the panel stands for the WHOLE segment, which for a sponsor is commonly 30-90 seconds, not the couple of
// seconds a prompt suggests. It is still the least costly of the sixteen - every other button either moves
// the playhead, opens a panel, or is the PS button - and the panel names it on screen while it is borrowed.
static int findSkipOffer(void)
{
   if (!state.sbReady || !state.player) return -1;
   float pos = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      const SponsorSegment *segment = &state.sb.segments[i];
      if (segment->skipped) continue;
      if (getSponsorAction(segment->category) != SPONSOR_ACTION_ASK) continue;
      if (pos >= segment->start && pos < segment->end - 0.2f) return i;
   }
   return -1;
}

// jump past the first not-yet-skipped sponsor segment the playhead has entered. one-shot per segment so a
// manual rewind into it doesn't fight the user.
static void autoSkipSponsor(void)
{
   float pos = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      SponsorSegment *segment = &state.sb.segments[i];
      if (!shouldAutoSkipSponsor(segment->category)) continue;
      if (segment->skipped || pos < segment->start || pos >= segment->end - 0.2f) continue;
      skipSegment(segment);
      return;
   }
}

// The resolve keeps the first 2047 bytes of a description (StreamInfo), so one that filled them was cut there:
// inside a word, maybe inside a character - and a broken character would send the whole text to FONT_POP. Such
// a text ends on its last whole word instead (on a whole character where no space is near, as in a long link)
// and in "...", like any text that does not fit - on a line of its own where the cut fell at a line's start.
// text has room for the 4 bytes this may add.
static void endCutText(char *text)
{
   int end = (int)strlen(text);
   int word = end;
   while (word > 0 && end - word < DESCRIPTION_CUT_REACH && text[word - 1] != ' ' && text[word - 1] != '\n') word--;
   if (word > 0 && end - word < DESCRIPTION_CUT_REACH) {
      end = word;
   } else {
      while (end > 0 && ((unsigned char)text[end - 1] & 0xC0) == 0x80) end--;   // back over a character's tail
      if (end > 0 && (unsigned char)text[end - 1] >= 0xC0) end--;                // and its first byte
   }
   int lineBreak = 0;
   while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\n')) {
      if (text[end - 1] == '\n') lineBreak = 1;
      end--;
   }
   strCopy(text + end, 5, lineBreak ? "\n..." : "...");
}

// Rasterises the description into descriptionTexture. It is text from YouTube like a title, so it is cleaned the
// same way first (uiCleanText, which keeps its line breaks): what no font draws - emoji, and decoration such as
// the pointer triangle (U+25BA) before each line of a list of links - is left out and has no say in the font.
// Each such triangle used to leave a blank em at the start of its line and to send the whole description to
// FONT_POP. Then the whole text in Manrope, or the whole text in FONT_POP when Manrope lacks a character still
// in it (Japanese, say).
//
// The renderer refuses a text whose wrapped surface would pass its 16 MB scratch limit (font.c) and leaves
// the texture empty. At the panel's text width that is about 86 lines of Manrope at 24 px - fewer than
// FONT_POP fits at the same size, as Manrope's line pitch is taller. So a text Manrope cannot fit is first
// tried whole in FONT_POP, which shows everything a description showed before v0.9. Only a text too long
// for FONT_POP as well - which used to leave the overlay blank - loses its last quarter (back to a line
// break where there is one), ends in "...", and is tried again; a refused attempt costs only the layout pass.
static void renderDescription(void)
{
   static char text[sizeof state.description + 8];   // + room for the "\n..." a shortened text ends in
   text[0] = '\0';
   if (state.description[0]) {
      strCopy(text, (int)sizeof text, state.description);
      if (state.descriptionCut) endCutText(text);
      uiCleanText(text, (int)sizeof text, text);
   }
   if (!text[0]) strCopy(text, (int)sizeof text, "No description.");   // none, or nothing a font can draw
   int width = DESCRIPTION_TEXT_W;
   Font *font = uiPickFont(DESCRIPTION_TEXT_WEIGHT, text);

   for (int cuts = 0; ; ) {
      renderFontRaw(&descriptionTexture, font, DESCRIPTION_TEXT_SIZE, text, activeTheme->textPrimary, width, TEXT_WRAP);
      int length = (int)strlen(text);
      if (descriptionTexture.tex.w > 0) {
         if (cuts > 0) logInfo("[yt] description shortened to %d bytes to fit the text renderer\n", length);
         return;
      }
      if (font != uiGetFallbackFont()) {   // too tall in Manrope: the whole of it in FONT_POP before any cut
         logInfo("[yt] description too long for Manrope (%d bytes), trying FONT_POP\n", length);
         font = uiGetFallbackFont();
         continue;
      }
      if (cuts >= DESCRIPTION_MAX_CUTS || length <= 64) {   // not the length, then (out of VRAM?): give up
         logError("[yt] description did not render (%d bytes)\n", length);
         return;
      }
      int cut = length * 3 / 4;
      int lineBreak = cut;
      while (lineBreak > 0 && text[lineBreak] != '\n') lineBreak--;
      if (lineBreak > 0) cut = lineBreak;
      else while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;   // never inside a character
      strCopy(text + cut, (int)sizeof text - cut, lineBreak > 0 ? "\n..." : "...");
      cuts++;
   }
}

// SELECT opens the description: rasterise the text once, off the draw path, as one tall wrapped
// texture the draw scrolls through. playback carries on untouched underneath.
static void openDescription(void)
{
   if (!state.descriptionRendered) {
      renderDescription();
      state.descriptionRendered = 1;
   }
   state.descriptionScroll = 0;
   state.descriptionVisible = 1;
}

// while the description is up it owns the pad: SELECT/circle close it, up/down scroll. playback
// input (seek, pause, back-out) is blocked underneath.
static void handleDescriptionInput(void)
{
   if (isPadButtonPressed(PAD_BTN_SELECT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      state.descriptionVisible = 0;
      return;
   }

   static ButtonRepeat scrollRepeat;
   if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.descriptionScroll += DESCRIPTION_SCROLL_STEP;
   else if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_UP))) state.descriptionScroll -= DESCRIPTION_SCROLL_STEP;

   int viewHeight = DESCRIPTION_VIEW_H;   // the panel's text window (drawDescription)
   int maxScroll = descriptionTexture.tex.h > viewHeight ? descriptionTexture.tex.h - viewHeight : 0;
   if (state.descriptionScroll > maxScroll) state.descriptionScroll = maxScroll;
   if (state.descriptionScroll < 0) state.descriptionScroll = 0;
}

// START opens the chapter picker: the row titles are rasterised once (off the draw path), the widest time
// and title size the two columns, and the selection starts on the chapter the playhead is in. The times are
// drawn in fixed-width digits (totalDigits), so they need no labels and line up as a column.
static void openChapters(void)
{
   // live streams can't seek, so a chapter jump has nowhere to go even if the description lists times
   if (state.chapters.count == 0 || state.isLive) { showToast("No chapters"); return; }

   if (!state.chaptersRendered) {
      int widestTitle = 0;
      for (int i = 0; i < state.chapters.count; i++) {
         char timeText[16];
         formatTime(timeText, sizeof timeText, (int)state.chapters.chapters[i].start);
         int timeWidth = uiGetDigitsWidth(&totalDigits, timeText);
         uiSetTextLabel(&chapterLabels[i], CHAPTER_TITLE_WEIGHT, state.chapters.chapters[i].title);
         if (timeWidth > state.chapterTimeWidth) state.chapterTimeWidth = timeWidth;
         if (chapterLabels[i].tt.tex.w > widestTitle) widestTitle = chapterLabels[i].tt.tex.w;
      }
      state.chapterPanelWidth = state.chapterTimeWidth + CHAPTER_TIME_GAP + widestTitle;
      state.chaptersRendered = 1;
   }

   state.chapterSelected = chapterAt(getVideoPositionSeconds(state.player));
   state.chaptersVisible = 1;
}

// while the picker is up it owns the pad: up/down move the selection, cross jumps to the chapter
// (playback carries on paused or playing, only the position moves), circle/START close without
// jumping. playback input is blocked underneath.
static void handleChaptersInput(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) { state.chaptersVisible = 0; return; }

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      state.chaptersVisible = 0;
      state.seeking = 0;   // a pending scrub must not overwrite the jump
      seekTo(state.chapters.chapters[state.chapterSelected].start, 1);   // in the chapter, as L1/R1 (jumpChapter)
      showControls();
      return;
   }

   static ButtonRepeat navigateRepeat;
   if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.chapterSelected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP))) state.chapterSelected--;
   if (state.chapterSelected < 0) state.chapterSelected = 0;
   if (state.chapterSelected >= state.chapters.count) state.chapterSelected = state.chapters.count - 1;
}

// ---- the HUD's text and layout (Bild 2) ----
// Labels are set here, in the update, and only rasterise when what they say changes; the draw only places
// them. Nothing here reads the pad.

// whole seconds still to go, rounded up: a countdown showing 0:00 while there is time left would read wrong
static int secondsUp(float seconds)
{
   if (seconds <= 0.0f) return 0;
   int whole = (int)seconds;
   return (float)whole < seconds ? whole + 1 : whole;
}

// the quality chip: the picked stream's short side, as YouTube names it ("1080p"; a portrait video by its
// width), with the frame rate above 30. Known once the worker picked a format; no chip without a height.
static void setQualityChip(void)
{
   int shortSide = (state.vidW > 0 && state.vidW < state.vidH) ? state.vidW : state.vidH;
   char text[16] = "";
   if (shortSide > 0) {
      if (state.vidFps > 30) snprintf(text, sizeof text, "%dp%d", shortSide, state.vidFps);
      else                   snprintf(text, sizeof text, "%dp", shortSide);
   }
   setLabelText(&qualityLabel, text);
}

// the audio chip, only on a video with several tracks: the track playing and its place in the list. The
// name is cut with "..." on its own label, so " · n/m" is never what gets cut.
static void setAudioChip(void)
{
   if (state.audioTrackCount <= 1) {
      uiSetTextLabel(&audioNameLabel, CHIP_TEXT_WEIGHT, "");
      setLabelText(&audioCountLabel, "");
      return;
   }
   const char *name = state.audioTracks[state.audioTrackPick].name;
   char count[24];
   if (name[0]) snprintf(count, sizeof count, " \xC2\xB7 %d/%d", state.audioTrackPick + 1, state.audioTrackCount);
   else         snprintf(count, sizeof count, "%d/%d", state.audioTrackPick + 1, state.audioTrackCount);
   uiSetTextLabel(&audioNameLabel, CHIP_TEXT_WEIGHT, name);
   setLabelText(&audioCountLabel, count);
}

// row A's chapter block: the chapter the playhead (or the scrub target) is in, and "· Chapter n of m" - by the
// position the time, the knob and L1/R1 go by (hudPosition), so the row names the chapter L1/R1 step from. A
// chapter jump lands on the keyframe at or before the chapter's start, so until the playhead reaches that
// start the row still names the chapter before, as L1/R1 then do. Re-set only when the chapter or the list
// changes - a scrub across chapters re-renders once per chapter.
static void updateChapterRow(void)
{
   int count = state.isLive ? 0 : state.chapters.count;   // live never shows chapters (openChapters refuses)
   int current = count > 0 ? chapterAt(hudPosition()) : -1;
   if (current == state.hudChapter && count == state.hudChapterCount) return;
   state.hudChapter = current;
   state.hudChapterCount = count;
   if (current < 0) return;
   char info[48];
   snprintf(info, sizeof info, " \xC2\xB7 Chapter %d of %d", current + 1, count);
   uiSetTextLabel(&chapterNameLabel, CHAPTER_NAME_WEIGHT, state.chapters.chapters[current].title);
   setLabelText(&chapterInfoLabel, info);
}

// The legend: every category among the fetched segments (all of them are marked on the timeline, whatever
// each is set to), in the order their colours first come along the timeline. Built once, when the
// segments arrive.
static void buildLegend(void)
{
   float firstStart[SPONSOR_CATEGORY_COUNT];
   int   seen[SPONSOR_CATEGORY_COUNT];
   for (int category = 0; category < SPONSOR_CATEGORY_COUNT; category++) { firstStart[category] = 0.0f; seen[category] = 0; }
   for (int i = 0; i < state.sb.count; i++) {
      const SponsorSegment *segment = &state.sb.segments[i];
      int category = (int)segment->category;
      if (category < 0 || category >= SPONSOR_CATEGORY_COUNT) continue;
      if (!seen[category] || segment->start < firstStart[category]) firstStart[category] = segment->start;
      seen[category] = 1;
   }

   state.legendCount = 0;
   for (int category = 0; category < SPONSOR_CATEGORY_COUNT; category++) {
      if (!seen[category]) continue;
      int at = state.legendCount++;
      while (at > 0 && firstStart[state.legendCategories[at - 1]] > firstStart[category]) {
         state.legendCategories[at] = state.legendCategories[at - 1];
         at--;
      }
      state.legendCategories[at] = (SponsorCategory)category;
   }

   state.legendWidth = 0;
   for (int i = 0; i < state.legendCount; i++) {
      uiInitLabel(&legendLabels[i], LEGEND_WEIGHT, 0, 0, AUTO, LEGEND_SIZE, activeTheme->textTertiary, TEXT_NOWRAP,
                  getSponsorCategoryName(state.legendCategories[i]));
      state.legendWidth += (i > 0 ? LEGEND_ITEM_GAP : 0) + LEGEND_DOT + LEGEND_DOT_GAP + legendLabels[i].tt.tex.w;
   }
   state.legendBuilt = 1;
}

// The hint row's width with the captions that change with the state at their widest ("Replay", "Subtitles"),
// and with Triangle counted even while a live stream hides it (a skip offer brings it back), so a pause or an
// offer never moves the legend between rows.
static int hintRowReserve(void)
{
   int triangleShown = !hudHints.hints[HINT_TRIANGLE].hidden;
   uiSetHintShown(&hudHints, HINT_TRIANGLE, 1);
   int width = uiGetHintsWidth(&hudHints);
   uiSetHintShown(&hudHints, HINT_TRIANGLE, triangleShown);
   width += state.crossCaptionMaxW - hudHints.hints[HINT_CROSS].caption.tt.tex.w;
   width += state.triangleCaptionMaxW - hudHints.hints[HINT_TRIANGLE].caption.tt.tex.w;
   return width;
}

// what row A's right end takes: the LIVE badge, or the time at its widest - the position as long as the
// duration
static int timeReserve(void)
{
   if (state.isLive) return uiGetBadgeWidth(&liveLabel);
   char duration[16], total[24];
   formatTime(duration, sizeof duration, (int)(getVideoDurationSeconds(state.player) + 0.5f));
   snprintf(total, sizeof total, " / %s", duration);
   return uiGetDigitsWidth(&timeDigits, duration) + uiGetDigitsWidth(&totalDigits, total);
}

// what row A's chapter block can take for this chapter list: its longest name (as far as the label lets it
// run) and "· Chapter m of m". Measured without rendering, once per chapter list.
static int chapterBlockReserve(void)
{
   if (state.isLive || state.chapters.count == 0) return 0;
   if (state.chapterReserveFor != state.chapters.count) {
      int widest = 0;
      for (int i = 0; i < state.chapters.count; i++) {
         // as chapterNameLabel shows it: cleaned (uiCleanText), which may take an emoji out and the title back
         // to Manrope with it
         char title[CHAPTER_TITLE_MAX];
         uiCleanText(title, (int)sizeof title, state.chapters.chapters[i].title);
         int width = (int)(measureFontText(uiPickFont(CHAPTER_NAME_WEIGHT, title), ROW_A_SIZE, title) + 0.999f);
         if (width > widest) widest = width;
      }
      if (widest > CHAPTER_NAME_MAX_W) widest = CHAPTER_NAME_MAX_W;
      char info[48];
      snprintf(info, sizeof info, " \xC2\xB7 Chapter %d of %d", state.chapters.count, state.chapters.count);
      state.chapterReserveW = widest + (int)(measureFontText(uiGetFont(CHAPTER_INFO_WEIGHT), ROW_A_SIZE, info) + 0.999f);
      state.chapterReserveFor = state.chapters.count;
   }
   return state.chapterReserveW;
}

// Fits the hint row into the safe width (Bild 2's gap first, closer only if the console's glyphs need it -
// RPCS3's are wider than the PS3's), then finds the legend a place: at the row's right end as in Bild 2; else
// on row A between the chapter block and the time; else on a line of its own above row A. Integer work
// only, so it runs every frame; it settles once the segments and chapters are in.
static void layoutLegend(void)
{
   int gap = HINT_ROW_GAP;
   uiSetHintsGap(&hudHints, gap);
   while (hintRowReserve() > HUD_W && gap > HINT_ROW_MIN_GAP) {
      gap -= 2;
      uiSetHintsGap(&hudHints, gap);
   }

   if (state.legendCount == 0) { state.legendPlace = LEGEND_NONE; return; }
   if (hintRowReserve() + LEGEND_GAP + state.legendWidth <= HUD_W) { state.legendPlace = LEGEND_IN_HINT_ROW; return; }
   int chapterW = chapterBlockReserve();
   if (chapterW + (chapterW > 0 ? ROW_A_GAP : 0) + state.legendWidth + ROW_A_GAP + timeReserve() <= HUD_W)
      state.legendPlace = LEGEND_IN_ROW_A;
   else
      state.legendPlace = LEGEND_OWN_LINE;
}

// The captions follow the state the input reads in the same update: Cross by pause and end, Square by the
// number of audio tracks, Triangle by the skip offer. On a live stream L1/R1 and L2/R2 do nothing, and START
// and Triangle can only answer "No chapters" and "No subtitles" (live counts as having neither), so those
// hints go - Triangle stays only as Skip, while an offer stands, the one thing it does there.
static void updateHintCaptions(void)
{
   const char *cross = isVideoEnded(state.player) ? "Replay" : isVideoPaused(state.player) ? "Play" : "Pause";
   uiSetHintCaption(&hudHints, HINT_CROSS, cross);
   uiSetHintCaption(&hudHints, HINT_SQUARE, state.audioTrackCount > 1 ? "Audio track" : "Autoplay");
   uiSetHintCaption(&hudHints, HINT_TRIANGLE, state.skipPrompt >= 0 ? "Skip" : "Subtitles");
   int seekable = !state.isLive;
   uiSetHintShown(&hudHints, HINT_L1, seekable);
   uiSetHintShown(&hudHints, HINT_R1, seekable);
   uiSetHintShown(&hudHints, HINT_L2, seekable);
   uiSetHintShown(&hudHints, HINT_R2, seekable);
   uiSetHintShown(&hudHints, HINT_START, seekable);
   uiSetHintShown(&hudHints, HINT_TRIANGLE, !state.isLive || state.skipPrompt >= 0);
}

static void updateHud(void)
{
   if (state.stage != STAGE_PLAYING || !state.player) return;
   updateChapterRow();
   if (state.sbReady && !state.legendBuilt) buildLegend();
   updateHintCaptions();
   layoutLegend();
}

static void updatePlay(void)
{
   // autoplay gets first refusal: a finished video hands the screen over before anything else runs
   if (state.stage == STAGE_PLAYING && state.player && !state.descriptionVisible && !state.chaptersVisible &&
       isVideoEnded(state.player) && advanceAutoplay()) return;

   // an overlay owns the pad for the whole frame it was (or just became) visible, so the button that
   // closed or opened it can't leak into playback input below (e.g. cross toggling pause on the way out)
   int overlayHadPad = state.descriptionVisible || state.chaptersVisible;
   if (state.descriptionVisible) handleDescriptionInput();
   else if (state.chaptersVisible) handleChaptersInput();
   else {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }   // back to the results list
      if (isPadButtonPressed(PAD_BTN_R3)) state.showStats = !state.showStats;
      // Square picks the audio track on a video that has several - its original language and YouTube's AI
      // dubs of it, say. Everywhere else it keeps toggling autoplay, which the settings screen also has.
      if (isPadButtonPressed(PAD_BTN_SQUARE)) {
         if (state.audioTrackCount > 1) cycleAudioTrack();
         else {
            setAutoplay(!getAutoplay());
            showToast(getAutoplay() ? "Autoplay on" : "Autoplay off");
         }
      }
      if (isPadButtonPressed(PAD_BTN_SELECT) && state.stage == STAGE_PLAYING && state.player) openDescription();
      if (isPadButtonPressed(PAD_BTN_START) && state.stage == STAGE_PLAYING && state.player) openChapters();
      // L3 is the only button still free in the player, and mid-video is exactly where you want the XMB on
      // a pad whose PS button the console never sees (DualShock 4 / DualSense). See ps-button.h.
      if (isPadButtonPressed(PAD_BTN_L3) && pressPsButton() != 0) showToast("PS button not available");
   }

   // An overlay that closed this frame hands UP/DOWN back to the volume, whose repeat timer still dates from
   // the meter's last step (or from never). isRepeatDue fires a HELD button at once on a stale timer, so a
   // direction still held from scrolling the description or walking the chapter list ran the volume down - or
   // up - the moment the overlay was gone. Stamped now, a direction held across the close waits out the
   // normal first-repeat delay as though it had just been pressed; a fresh press still steps at once.
   if (overlayHadPad && !state.descriptionVisible && !state.chaptersVisible) {
      volumeMeter.repeat.timer   = sys_time_get_system_time();
      volumeMeter.repeat.repeats = 0;
   }

   // reap the worker once it's done; adopt the player it built
   if (state.workerDone && state.threadActive) {
      joinThread(state.workerTid);
      state.threadActive = 0;
      if (state.pendingPlayer) {
         state.player = state.pendingPlayer;
         state.pendingPlayer = NULL;
         setAudioPcmFeedVolume(getVolumeMeterFraction(&volumeMeter));
         uiSetTextLabel(&titleLabel, TITLE_WEIGHT, state.title);   // rasterise once, off the draw path
         state.stage = STAGE_PLAYING;
         if (state.switchPending) {   // an audio switch landed: keep the paused state, and name the track
            setVideoPaused(state.player, state.switchWasPaused);
            if (state.switchReverting)
               snprintf(state.audioNotice, sizeof state.audioNotice, "Track unavailable - %s",
                        state.audioTracks[state.switchTarget].name);
            else
               snprintf(state.audioNotice, sizeof state.audioNotice, "Audio %d/%d: %s", state.switchTarget + 1,
                        state.audioTrackCount, state.audioTracks[state.switchTarget].name);
            state.audioTrackPick = state.switchTarget;
            state.audItag = state.audioTracks[state.audioTrackPick].itag;
            state.switchPending = 0;
            state.audioNoticePending = 1;
         } else if (state.reopenPending) {   // the rebuild after the end (reopenAt): it just plays on, no notice
            state.reopenPending = 0;
         } else if (state.audioTrackCount > 1) {
            // say once, as the video starts, that it has other tracks and which button reaches them - the
            // complaint this answers was exactly that there was no way to know or to change it
            snprintf(state.audioNotice, sizeof state.audioNotice, "Audio: %s  -  Square: %d tracks",
                     state.audioTracks[state.audioTrackPick].name, state.audioTrackCount);
            state.audioNoticePending = 1;
         }
         // the chips, now the formats and the track playing are settled. The subtitle chip only on the first
         // adoption: after an audio switch it still names the pick, which the switch leaves alone.
         setQualityChip();
         setAudioChip();
         if (subsHudLabel.text[0] == '\0' && state.captionCount > 0 && !state.isLive)
            uiSetTextLabel(&subsHudLabel, CHIP_TEXT_WEIGHT, "Subtitles off");
      } else if (state.reopenPending) {
         state.reopenPending = 0;
         fail("couldn't reopen the video");
      } else if (state.switchPending && !state.switchReverting) {
         // the new track would not open, and the old player is gone: rebuild the track we came from, once
         logError("[yt] audio track %s did not open, going back\n", state.audioTracks[state.switchTarget].name);
         state.switchReverting = 1;
         state.switchTarget = state.audioTrackPick;
         startSwitchWorker();
      } else if (state.switchPending) {
         state.switchPending = 0;
         fail("couldn't reopen the video");
      }
   }

   // the audio notice waits for the picture: a rebuild's seek reload can outlast the toast otherwise
   if (state.audioNoticePending && state.firstFrameSeen && state.stage == STAGE_PLAYING) {
      showToast(state.audioNotice);
      state.audioNoticePending = 0;
   }

   // reap the sponsorblock fetch; its segments are safe to read once joined
   if (state.sbWorkerDone && state.sbThreadActive) {
      joinThread(state.sbWorkerTid);
      state.sbThreadActive = 0;
      state.sbReady = 1;
   }

   // chapters: when the description parse found none, ask /next once for youtube's own list
   if (state.stage == STAGE_PLAYING && !state.chFetchTried && state.chapters.count == 0 && !state.isLive &&
       isBareVideoId(requestedInput)) {
      state.chFetchTried = 1;
      state.chThreadActive = (spawnJoinableThread(&state.chWorkerTid, chapterWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-chapters") == 0);
   }
   if (state.chWorkerDone && state.chThreadActive) {
      joinThread(state.chWorkerTid);
      state.chThreadActive = 0;
      state.chapters = state.fetchedChapters;   // safe to adopt now the worker is reaped
   }

   // subtitles: reap a finished fetch (stale if the pick moved mid-fetch), then start one whenever
   // the pick and the fetched track disagree
   if (state.subWorkerDone && state.subThreadActive) {
      joinThread(state.subWorkerTid);
      state.subThreadActive = 0;
      state.subtitlesReady = (state.subtitleFetched == state.subtitlePick);
      if (!state.subtitlesReady) state.subtitleFetched = -1;   // pick moved mid-fetch: re-picking must refetch
      else if (state.subtitleTrack->count == 0) {   // fetch failed or empty track: drop to Off, visibly
         state.subtitlePick = state.subtitleFetched = -1;
         state.subtitlesReady = 0;
         uiSetTextLabel(&subsHudLabel, CHIP_TEXT_WEIGHT, "Subtitles off");
         showToast("Subtitles unavailable");
      }
   }
   if (state.subtitlePick >= 0 && state.subtitlePick != state.subtitleFetched && !state.subThreadActive) {
      if (!state.subtitleTrack) state.subtitleTrack = malloc(sizeof *state.subtitleTrack);
      if (state.subtitleTrack) {
         state.subtitlesReady = 0;
         state.subtitleScanFrom = 0;
         state.subWorkerDone = 0;
         state.subtitleFetched = state.subtitlePick;
         state.subThreadActive = (spawnJoinableThread(&state.subWorkerTid, subtitleWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-subs") == 0);
         if (!state.subThreadActive) state.subtitleFetched = -1;   // spawn failed: retry next frame
      }
   }

   // feed the cue covering the current position into the label; setLabelText only rasterises on change
   if (state.subtitlesReady && state.subtitlePick >= 0 && state.stage == STAGE_PLAYING && state.player)
      setLabelText(&subtitleLabel, getSubtitleCueText(state.subtitleTrack, getVideoPositionSeconds(state.player), &state.subtitleScanFrom));

   // The manual offer is recomputed BEFORE input is read, not after. Computing it afterwards left Triangle
   // judged against the previous frame's answer: on the first frame after the playhead left an "ask"
   // segment - or after scrubbing out of one - a Triangle meant for the subtitles would still have run the
   // skip and jumped the video forward to that segment's end. It also has to clear itself when the segment
   // ends, so it is recomputed every frame rather than latched.
   state.skipPrompt = (state.stage == STAGE_PLAYING && !state.descriptionVisible && !state.chaptersVisible)
                      ? findSkipOffer() : -1;
   if (state.skipPrompt >= 0 && state.skipPrompt != state.skipPromptShown) {
      const SponsorSegment *segment = &state.sb.segments[state.skipPrompt];
      char offer[64], length[16];
      snprintf(offer, sizeof offer, "Skip %s", getSponsorCategoryName(segment->category));
      setLabelText(&skipPromptLabel, offer);
      // the panel's countdown is sized for the whole segment, so the panel keeps its width as it counts
      formatTime(length, sizeof length, secondsUp(segment->end - segment->start));
      state.skipTimeReserveW = uiGetDigitsWidth(&skipDigits, length);
      state.skipPromptShown = state.skipPrompt;
   }

   if (state.stage == STAGE_PLAYING && state.player && !overlayHadPad && !state.descriptionVisible && !state.chaptersVisible)
      handlePlaybackInput();
   if (state.sbReady && state.stage == STAGE_PLAYING && state.player && !isVideoPaused(state.player) && !state.seeking)
      autoSkipSponsor();

   // after the input, so the hint captions already say what the buttons do after this frame's presses
   updateHud();

   // status text: the failure reason, or "Loading..." until playback. setLabelText skips unchanged
   // text, so calling it each frame only rasterises on a real change.
   if (state.stage == STAGE_FAILED) setLabelText(&statusLabel, state.message);
   else if (state.stage != STAGE_PLAYING) setLabelText(&statusLabel, "Loading...");
}

// measure the real presented frame rate: a changed frame pointer is a new frame; tally them per second.
static void measureFps(const uint8_t *frame)
{
   if (frame && frame != state.lastFrame) { state.framesThisSecond++; state.lastFrame = frame; }
   uint64_t now = sys_time_get_system_time();
   if (now - state.fpsTickUs >= 1000000) { state.measuredFps = state.framesThisSecond; state.framesThisSecond = 0; state.fpsTickUs = now; }
}

// rebuild the overlay lines. setLabelText skips unchanged text, so calling this per frame only
// rasterises when a value actually moves (fps + time once a second).
static void updateStatLabels(void)
{
   char line[96];
   int rate = 0, channels = 0;
   getAudioTrackInfo(state.player, &rate, &channels);
   float pos = getVideoPositionSeconds(state.player), duration = getVideoDurationSeconds(state.player);

   snprintf(line, sizeof line, "Video  itag %d   %dx%d   H.264", state.vidItag, state.vidW, state.vidH);
   setLabelText(&statLabels[0], line);
   snprintf(line, sizeof line, "FPS  %d   (target %d)", state.measuredFps, state.vidFps);
   setLabelText(&statLabels[1], line);
   if (state.audItag) snprintf(line, sizeof line, "Audio  itag %d   AAC %d Hz   %d ch", state.audItag, rate, channels);
   else               strCopy(line, sizeof line, "Audio  none");
   setLabelText(&statLabels[2], line);
   // the times as row A writes them (formatTime: h:mm:ss past an hour), so the panel and the HUD under it
   // name one duration the same way; a live stream has no duration, only how far it has run
   char now[16], length[16];
   formatTime(now, sizeof now, (int)pos);
   formatTime(length, sizeof length, (int)(duration + 0.5f));
   if (state.isLive) snprintf(line, sizeof line, "Time  %s", now);
   else              snprintf(line, sizeof line, "Time  %s / %s", now, length);
   setLabelText(&statLabels[3], line);
}

// the stats panel sits under the title block, so the two never overlap while the controls are up, and
// above the volume plate
static void drawStatsOverlay(void)
{
   int x = HUD_X, y = STATS_Y;
   uiFillRounded(x, y, STATS_PANEL_W, STAT_LINES * STATS_LINE_H + 2 * STATS_PAD_Y, UI_RADIUS_PANEL,
                 activeTheme->panelOnVideo);
   for (int i = 0; i < STAT_LINES; i++) {
      int lineY = y + STATS_PAD_Y + i * STATS_LINE_H;
      uiDrawLabelAt(&statLabels[i], x + STATS_PAD_X, uiLabelYForCenter(&statLabels[i], lineY, STATS_LINE_H));
   }
}

// ---- drawing the HUD (Bild 2) ----

// Bild 2's two gradients, only with the HUD: dark at the top edge, fading out over 260 px, and from y 620
// down to nearly opaque at the bottom edge, behind the chapter row, the timeline and the hints
static void drawGradients(void)
{
   uint32_t base = activeTheme->gradientBase;
   uiFillGradient2V(0, 0, state.screenW, TOP_GRADIENT_H, uiWithAlpha(base, 0xC7), uiWithAlpha(base, 0x00));
   UiGradientStop bottom[3];
   bottom[0].at = 0.00f; bottom[0].argb = uiWithAlpha(base, 0x00);
   bottom[1].at = 0.45f; bottom[1].argb = uiWithAlpha(base, 0xB3);
   bottom[2].at = 1.00f; bottom[2].argb = uiWithAlpha(base, 0xF0);
   uiFillGradientV(0, BOTTOM_GRADIENT_Y, state.screenW, state.screenH - BOTTOM_GRADIENT_Y, bottom, 3);
}

// the meta line's width: "channel · views", or whichever of the two the entry has (0 for neither)
static int metaLineWidth(void)
{
   int width = channelLabel.tt.tex.w > 0 ? channelLabel.tt.tex.w : 0;
   if (width > 0 && viewsLabel.tt.tex.w > 0) width += META_GAP + metaDotLabel.tt.tex.w + META_GAP;
   if (viewsLabel.tt.tex.w > 0) width += viewsLabel.tt.tex.w;
   return width;
}

// the title plate's right edge (HUD_X without a title), which the toast keeps clear of
static int titlePlateRight(void)
{
   return HUD_X + (titleLabel.tt.tex.w > 0 ? uiGetTextBoxWidth(&titleLabel, TITLE_PLATE_PAD_X) : 0);
}

// Top left: "channel · views" on a chip in the top-right chips' row and look, when the video came from a
// list, and the title on a plate of the same fill under it. Each label's capitals are centred by its own
// font's measure, so a channel name that fell back to FONT_POP stays level with the views beside it.
static void drawTitleBlock(void)
{
   int metaW = metaLineWidth();
   if (metaW > 0) {
      uiFillRounded(HUD_X, CHIP_Y, metaW + 2 * CHIP_PAD_X, CHIP_H, CHIP_RADIUS, activeTheme->chipOnVideo);
      int x = HUD_X + CHIP_PAD_X;
      if (channelLabel.tt.tex.w > 0) {
         uiDrawLabelAt(&channelLabel, x, uiLabelYForCenter(&channelLabel, CHIP_Y, CHIP_H));
         x += channelLabel.tt.tex.w + META_GAP;
         if (viewsLabel.tt.tex.w > 0) {
            uiDrawLabelAt(&metaDotLabel, x, uiLabelYForCenter(&metaDotLabel, CHIP_Y, CHIP_H));
            x += metaDotLabel.tt.tex.w + META_GAP;
         }
      }
      if (viewsLabel.tt.tex.w > 0) uiDrawLabelAt(&viewsLabel, x, uiLabelYForCenter(&viewsLabel, CHIP_Y, CHIP_H));
   }
   if (titleLabel.tt.tex.w > 0)
      uiDrawTextBox(HUD_X, TITLE_PLATE_Y, TITLE_PLATE_H, TITLE_PLATE_PAD_X, TITLE_PLATE_RADIUS, &titleLabel,
                    activeTheme->chipOnVideo);
}

static int audioChipWidth(void)
{
   if (state.audioTrackCount <= 1 || audioCountLabel.tt.tex.w <= 0) return 0;
   return 2 * CHIP_PAD_X + audioNameLabel.tt.tex.w + audioCountLabel.tt.tex.w;
}

// The audio chip in Bild 2's accent look: accentSoft, an accent ring, text in accentText. The 20 % accent on
// its own turns into a pale wash on a white picture, which accentText cannot be read on, so it is laid over
// the dark fill the other chips have.
static void drawAudioChip(int x, int w)
{
   uiFillRounded(x, CHIP_Y, w, CHIP_H, AUDIO_CHIP_RADIUS, activeTheme->chipOnVideo);
   uiFillRounded(x, CHIP_Y, w, CHIP_H, AUDIO_CHIP_RADIUS, activeTheme->accentSoft);
   uiDrawNineSlice(UI_TEX_RING_R20_W3, x, CHIP_Y, w, CHIP_H, AUDIO_RING_SCALE,
                   uiWithAlpha(activeTheme->accent, AUDIO_RING_ALPHA));
   int textX = x + CHIP_PAD_X;
   uiDrawLabelAt(&audioNameLabel, textX, uiLabelYForCenter(&audioNameLabel, CHIP_Y, CHIP_H));
   uiDrawLabelAt(&audioCountLabel, textX + audioNameLabel.tt.tex.w, uiLabelYForCenter(&audioCountLabel, CHIP_Y, CHIP_H));
}

// Top right, laid out from the right edge leftwards - read left to right: quality, audio track, subtitles.
// A chip shows only when its value is known (its label has text).
static void drawChips(void)
{
   int x = HUD_RIGHT;
   if (subsHudLabel.tt.tex.w > 0) {
      x -= uiGetTextBoxWidth(&subsHudLabel, CHIP_PAD_X);
      uiDrawTextBox(x, CHIP_Y, CHIP_H, CHIP_PAD_X, CHIP_RADIUS, &subsHudLabel, activeTheme->chipOnVideo);
      x -= CHIP_GAP;
   }
   int audioW = audioChipWidth();
   if (audioW > 0) {
      x -= audioW;
      drawAudioChip(x, audioW);
      x -= CHIP_GAP;
   }
   if (qualityLabel.tt.tex.w > 0) {
      x -= uiGetTextBoxWidth(&qualityLabel, CHIP_PAD_X);
      uiDrawTextBox(x, CHIP_Y, CHIP_H, CHIP_PAD_X, CHIP_RADIUS, &qualityLabel, activeTheme->chipOnVideo);
   }
}

// Row A: the chapter block on the left (with chapters only), the time on the right - or, on a live stream,
// the LIVE badge where the time would be. The times are fixed-width digits; text on the gradient is
// outlined.
static void drawRowA(float position, float duration)
{
   uint32_t outline = activeTheme->gradientBase;
   if (state.isLive) {
      uiDrawBadge(HUD_RIGHT - uiGetBadgeWidth(&liveLabel), LIVE_BADGE_Y, &liveLabel, activeTheme->live);
   } else {
      char now[16], length[16], total[24];
      formatTime(now, sizeof now, (int)position);
      formatTime(length, sizeof length, (int)(duration + 0.5f));
      snprintf(total, sizeof total, " / %s", length);
      int totalX = HUD_RIGHT - uiGetDigitsWidth(&totalDigits, total);
      uiDrawDigitsOutlined(&timeDigits, totalX - uiGetDigitsWidth(&timeDigits, now),
                           uiTextYForBaseline(timeDigits.font, timeDigits.size, ROW_A_BASELINE), now,
                           activeTheme->textPrimary, outline, TEXT_OUTLINE);
      uiDrawDigitsOutlined(&totalDigits, totalX, uiTextYForBaseline(totalDigits.font, totalDigits.size, ROW_A_BASELINE),
                           total, activeTheme->textSecondary, outline, TEXT_OUTLINE);
   }
   if (state.hudChapter >= 0) {
      uiPlaceLabelBaseline(&chapterNameLabel, HUD_X, ROW_A_BASELINE);
      uiDrawLabelOutlined(&chapterNameLabel, outline, TEXT_OUTLINE);
      uiPlaceLabelBaseline(&chapterInfoLabel, HUD_X + chapterNameLabel.tt.tex.w, ROW_A_BASELINE);
      uiDrawLabelOutlined(&chapterInfoLabel, outline, TEXT_OUTLINE);
   }
}

// x of a time on the timeline: one linear map for the knob, the played part, the chapter gaps and the
// sponsor marks, so they all agree on where a second is
static int timelineX(float seconds, float duration)
{
   if (duration <= 0.0f) return HUD_X;
   float fraction = seconds / duration;
   if (fraction < 0.0f) fraction = 0.0f;
   if (fraction > 1.0f) fraction = 1.0f;
   return HUD_X + (int)(fraction * HUD_W + 0.5f);
}

typedef struct { int x0, x1; } TimelinePiece;

// The track as one piece per chapter, with a 5-px gap where one chapter ends and the next begins (2 px of it
// before the boundary, 3 after) in place of the old notches; without chapters, one piece from end to end. A
// chapter the bar cannot place - out of order, or starting past the end (the /next list is not checked
// against the duration) - makes no gap, and one too short to outlast its gap leaves no piece.
static int buildTimelinePieces(TimelinePiece *pieces, float duration)
{
   int count = 0, left = HUD_X;
   float last = 0.0f;
   for (int i = 0; i < state.chapters.count && duration > 0.0f; i++) {
      float start = state.chapters.chapters[i].start;
      if (start <= last || start >= duration) continue;
      int boundary = timelineX(start, duration);
      if (boundary - 2 > left) { pieces[count].x0 = left; pieces[count].x1 = boundary - 2; count++; }
      left = boundary + 3;
      last = start;
   }
   if (HUD_RIGHT > left) { pieces[count].x0 = left; pieces[count].x1 = HUD_RIGHT; count++; }
   return count;
}

// The timeline: each piece in seekTrack, the played part over it in the accent, the sponsor segments over
// both in their category colours (as SponsorBlock marks them on YouTube, so a skipped stretch stays visible
// in what has been watched), and the knob on top with its halo and glow. Every piece is one rounded 9-slice.
static void drawTimeline(float position, float duration)
{
   TimelinePiece pieces[MAX_CHAPTERS + 1];
   int count = buildTimelinePieces(pieces, duration);
   int playedX = timelineX(position, duration);

   for (int i = 0; i < count; i++) {
      int x0 = pieces[i].x0, x1 = pieces[i].x1;
      if (playedX >= x1) { uiFillRounded(x0, TRACK_Y, x1 - x0, TRACK_H, TRACK_RADIUS, activeTheme->accent); continue; }
      uiFillRounded(x0, TRACK_Y, x1 - x0, TRACK_H, TRACK_RADIUS, activeTheme->seekTrack);
      // the played part's right end lies under the knob, so its rounding never shows
      if (playedX > x0) uiFillRounded(x0, TRACK_Y, playedX - x0, TRACK_H, TRACK_RADIUS, activeTheme->accent);
   }

   if (state.sbReady && duration > 0.0f)
      for (int s = 0; s < state.sb.count; s++) {
         const SponsorSegment *segment = &state.sb.segments[s];
         int sx0 = timelineX(segment->start, duration), sx1 = timelineX(segment->end, duration);
         if (sx1 - sx0 < SPONSOR_MIN_W) { sx0 = (sx0 + sx1) / 2 - SPONSOR_MIN_W / 2; sx1 = sx0 + SPONSOR_MIN_W; }
         // cut to the pieces, so a segment across a chapter boundary leaves the gap a gap
         for (int i = 0; i < count; i++) {
            int a = sx0 > pieces[i].x0 ? sx0 : pieces[i].x0;
            int b = sx1 < pieces[i].x1 ? sx1 : pieces[i].x1;
            if (b > a) uiFillRounded(a, TRACK_Y, b - a, TRACK_H, TRACK_RADIUS, getSponsorCategoryColor(segment->category));
         }
      }

   uiDrawGlow(playedX - KNOB_D / 2, KNOB_CY - KNOB_D / 2, KNOB_D, KNOB_D, KNOB_GLOW_SCALE, activeTheme->knobGlow);
   uiFillCircleAt(playedX, KNOB_CY, KNOB_HALO_D, activeTheme->accentSoft);
   uiFillCircleAt(playedX, KNOB_CY, KNOB_D, activeTheme->accent);
}

// the SponsorBlock legend where layoutLegend put it: a dot in each category's colour, then its name
static void drawLegend(void)
{
   if (state.legendPlace == LEGEND_NONE) return;
   Font *font = uiGetFont(LEGEND_WEIGHT);
   int x, labelY;
   if (state.legendPlace == LEGEND_IN_HINT_ROW) {
      x = HUD_RIGHT - state.legendWidth;
      labelY = uiTextYForCenter(font, LEGEND_SIZE, HINT_ROW_Y, UI_HINT_GLYPH_H);
   } else if (state.legendPlace == LEGEND_IN_ROW_A) {
      x = HUD_RIGHT - timeReserve() - ROW_A_GAP - state.legendWidth;
      labelY = uiTextYForBaseline(font, LEGEND_SIZE, ROW_A_BASELINE);
   } else {
      x = HUD_X;
      labelY = uiTextYForBaseline(font, LEGEND_SIZE, LEGEND_LINE_BASELINE);
   }
   UiTextMetrics metrics = uiGetTextMetrics(font, LEGEND_SIZE);
   int dotCenterY = labelY + (metrics.capTop + metrics.baseline) / 2;   // the dots sit on the capitals' middle
   for (int i = 0; i < state.legendCount; i++) {
      uiFillCircleAt(x + LEGEND_DOT / 2, dotCenterY, LEGEND_DOT, getSponsorCategoryColor(state.legendCategories[i]));
      x += LEGEND_DOT + LEGEND_DOT_GAP;
      moveLabel(&legendLabels[i], x, labelY);
      uiDrawLabelOutlined(&legendLabels[i], activeTheme->gradientBase, TEXT_OUTLINE);
      x += legendLabels[i].tt.tex.w + LEGEND_ITEM_GAP;
   }
}

// the whole HUD, in Bild 2's paint order: gradients, title block, chips, row A, timeline, hints and legend
static void drawHud(void)
{
   float duration = getVideoDurationSeconds(state.player);
   float position = hudPosition();
   drawGradients();
   drawTitleBlock();
   drawChips();
   drawRowA(position, duration);
   if (!state.isLive) drawTimeline(position, duration);   // a live stream has no timeline to show
   uiDrawHints(&hudHints, HUD_X, HINT_ROW_Y, UI_ALIGN_LEFT);
   drawLegend();
}

// the HUD shows while scrubbing / paused / ended, else auto-hides after the last activity
static int controlsVisible(void)
{
   if (isVideoPaused(state.player) || isVideoEnded(state.player) || state.seeking) return 1;
   return state.controlsShownUs != 0 && sys_time_get_system_time() - state.controlsShownUs < CONTROLS_VISIBLE_US;
}

// The active subtitle cue, centred low on the picture on a dark plate. While the HUD is up it moves above
// the bottom block - and above the skip offer as well when that stands, as a wide cue would run into it.
static void drawSubtitle(int hudUp)
{
   if (state.subtitlePick < 0 || subtitleLabel.tt.tex.w == 0) return;
   int bottom = SUBTITLE_BOTTOM;
   if (hudUp) {
      bottom = (state.legendPlace == LEGEND_OWN_LINE ? LEGEND_LINE_TOP : ROW_A_TOP) - SUBTITLE_HUD_GAP;
      if (state.skipPrompt >= 0 && bottom > SKIP_Y - SUBTITLE_HUD_GAP) bottom = SKIP_Y - SUBTITLE_HUD_GAP;
   }
   int w = subtitleLabel.tt.tex.w, h = subtitleLabel.tt.tex.h;
   int x = state.screenW / 2 - w / 2, y = bottom - h;
   uiFillRounded(x - SUBTITLE_PAD_X, y - SUBTITLE_PAD_Y, w + 2 * SUBTITLE_PAD_X, h + 2 * SUBTITLE_PAD_Y, SUBTITLE_RADIUS,
                 activeTheme->panelOnVideo);
   uiDrawLabelAt(&subtitleLabel, x, y);
}

// ---- the overlays ----

// an overlay's panel: surface1, its heading on the left of the top band and that overlay's own buttons on
// the right of it, a hairline under the two
static void drawOverlayPanel(int x, int y, int w, int h, const Label *heading, const UiHints *hints)
{
   uiFillRounded(x, y, w, h, UI_RADIUS_PANEL, activeTheme->surface1);
   uiDrawLabelAt(heading, x + PANEL_PAD_X, uiLabelYForCenter(heading, y, PANEL_HEADER_H));
   uiDrawHints(hints, x + w - PANEL_PAD_X, y + (PANEL_HEADER_H - UI_HINT_GLYPH_H) / 2, UI_ALIGN_RIGHT);
   uiFillRect(x + PANEL_PAD_X, y + PANEL_HEADER_H - 2, w - 2 * PANEL_PAD_X, 2, activeTheme->hairline);
}

// a slim scroll bar beside a window of `height` px onto `total` px: where the window is, and how much of the
// whole it shows
static void drawScrollBar(int x, int y, int height, int offset, int maxOffset, int total)
{
   if (maxOffset <= 0 || total <= 0) return;
   uiFillPill(x, y, SCROLLBAR_W, height, activeTheme->hairline);
   int thumbH = height * height / total;
   if (thumbH < 40) thumbH = 40;
   if (thumbH > height) thumbH = height;
   uiFillPill(x, y + (height - thumbH) * offset / maxOffset, SCROLLBAR_W, thumbH, activeTheme->textTertiary);
}

// The description on a panel over the dimmed video: the slice of the text texture the scroll position
// selects (the texture is drawn through a window, so no clipping is needed), fading into the panel at an
// edge where more text runs on, and a scroll bar beside it when there is more than one window of it.
static void drawDescription(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
   drawOverlayPanel(DESCRIPTION_PANEL_X, DESCRIPTION_PANEL_Y, DESCRIPTION_PANEL_W, DESCRIPTION_PANEL_H,
                    &descriptionHeading, &descriptionHints);
   if (descriptionTexture.tex.w == 0) return;

   int visibleHeight = descriptionTexture.tex.h - state.descriptionScroll;
   if (visibleHeight > DESCRIPTION_VIEW_H) visibleHeight = DESCRIPTION_VIEW_H;
   if (visibleHeight <= 0) return;

   float sliceTop    = (float)state.descriptionScroll / descriptionTexture.tex.h;
   float sliceBottom = (float)(state.descriptionScroll + visibleHeight) / descriptionTexture.tex.h;
   drawGfxTexture(DESCRIPTION_VIEW_X, DESCRIPTION_VIEW_Y, descriptionTexture.tex.w, visibleHeight,
                  descriptionTexture.tex, 0.0f, sliceTop, 1.0f, sliceBottom, 0xFFFFFFFF, GFX_FILTER_NEAREST);

   int maxScroll = descriptionTexture.tex.h - DESCRIPTION_VIEW_H;
   if (maxScroll <= 0) return;
   uint32_t panel = activeTheme->surface1;
   if (state.descriptionScroll > 0)
      uiFillGradient2V(DESCRIPTION_VIEW_X, DESCRIPTION_VIEW_Y, DESCRIPTION_TEXT_W, DESCRIPTION_FADE_H,
                       panel, uiWithAlpha(panel, 0x00));
   if (state.descriptionScroll < maxScroll)
      uiFillGradient2V(DESCRIPTION_VIEW_X, DESCRIPTION_VIEW_Y + DESCRIPTION_VIEW_H - DESCRIPTION_FADE_H, DESCRIPTION_TEXT_W,
                       DESCRIPTION_FADE_H, uiWithAlpha(panel, 0x00), panel);
   drawScrollBar(DESCRIPTION_PANEL_X + DESCRIPTION_PANEL_W - PANEL_PAD_X - SCROLLBAR_W, DESCRIPTION_VIEW_Y,
                 DESCRIPTION_VIEW_H, state.descriptionScroll, maxScroll, descriptionTexture.tex.h);
}

// The chapter picker on a panel: rows windowed around the selection, the start times in fixed-width digits
// right-aligned in a column of their own, the titles after them. The selected row lies on an accent plate,
// its time in the accent's text colour.
static void drawChapters(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);

   // window the rows around the selection so long lists stay reachable without a scrollbar
   int rows = state.chapters.count < CHAPTER_VISIBLE_ROWS ? state.chapters.count : CHAPTER_VISIBLE_ROWS;
   int firstRow = state.chapterSelected - rows / 2;
   if (firstRow > state.chapters.count - rows) firstRow = state.chapters.count - rows;
   if (firstRow < 0) firstRow = 0;

   int panelW = state.chapterPanelWidth + 2 * PANEL_PAD_X;
   if (panelW < CHAPTER_PANEL_MIN_W) panelW = CHAPTER_PANEL_MIN_W;
   if (panelW > CHAPTER_PANEL_MAX_W) panelW = CHAPTER_PANEL_MAX_W;
   int panelH = PANEL_HEADER_H + CHAPTER_LIST_PAD_TOP + rows * CHAPTER_ROW_HEIGHT + CHAPTER_LIST_PAD_BOTTOM;
   int panelX = state.screenW / 2 - panelW / 2, panelY = state.screenH / 2 - panelH / 2;
   drawOverlayPanel(panelX, panelY, panelW, panelH, &chaptersHeading, &chapterHints);

   int timeRight = panelX + PANEL_PAD_X + state.chapterTimeWidth;
   int titleX    = timeRight + CHAPTER_TIME_GAP;
   int topY      = panelY + PANEL_HEADER_H + CHAPTER_LIST_PAD_TOP;
   int plateX    = panelX + PANEL_PAD_X - CHAPTER_ROW_INSET;
   int plateW    = panelW - 2 * (PANEL_PAD_X - CHAPTER_ROW_INSET);

   for (int row = 0; row < rows; row++) {
      int index    = firstRow + row;
      int rowY     = topY + row * CHAPTER_ROW_HEIGHT;
      int selected = index == state.chapterSelected;
      if (selected) uiFillRounded(plateX, rowY + 4, plateW, CHAPTER_ROW_HEIGHT - 8, UI_RADIUS_ROW, activeTheme->rowHighlight);
      char timeText[16];
      formatTime(timeText, sizeof timeText, (int)state.chapters.chapters[index].start);
      uiDrawDigits(&totalDigits, timeRight - uiGetDigitsWidth(&totalDigits, timeText),
                   uiTextYForCenter(totalDigits.font, totalDigits.size, rowY, CHAPTER_ROW_HEIGHT), timeText,
                   selected ? activeTheme->accentText : activeTheme->textSecondary);
      // capitals centred by each label's own measured font: a title that fell back to FONT_POP stays level
      // with its Manrope time
      uiDrawLabelAt(&chapterLabels[index], titleX, uiLabelYForCenter(&chapterLabels[index], rowY, CHAPTER_ROW_HEIGHT));
   }
}

// a brief toast at the top right: under the chips - or under the title plate, when it would reach the plate
static void drawToast(void)
{
   if (state.toastShownUs == 0 || sys_time_get_system_time() - state.toastShownUs >= TOAST_VISIBLE_US) return;
   if (toastLabel.tt.tex.w <= 0) return;
   int x = HUD_RIGHT - uiGetTextBoxWidth(&toastLabel, TOAST_PAD_X);
   int y = x >= titlePlateRight() + TOAST_TITLE_GAP ? TOAST_Y : TOAST_Y_LOW;
   uiDrawTextBox(x, y, TOAST_H, TOAST_PAD_X, TOAST_RADIUS, &toastLabel, activeTheme->panelOnVideo);
}

// The offer panel (Bild 2): the console's Triangle on a disc, "Skip <category>", and under it the category's
// colour and the time left in the segment. On the right, where the eye is not busy with the picture's
// centre. Drawn whenever an offer stands, controls up or not - a skip button that hides with the controls
// is a skip button you miss.
static void drawSkipPrompt(void)
{
   if (state.skipPrompt < 0) return;
   const SponsorSegment *segment = &state.sb.segments[state.skipPrompt];

   char left[16];
   formatTime(left, sizeof left, secondsUp(segment->end - getVideoPositionSeconds(state.player)));
   int subW    = SKIP_SUB_DOT + SKIP_SUB_DOT_GAP + state.skipTimeReserveW + skipSuffixLabel.tt.tex.w;
   int columnW = skipPromptLabel.tt.tex.w > subW ? skipPromptLabel.tt.tex.w : subW;
   int panelW  = SKIP_TEXT_DX + columnW + SKIP_PAD_R;
   int x = HUD_RIGHT - panelW, y = SKIP_Y;

   uiFillRounded(x, y, panelW, SKIP_H, SKIP_RADIUS, activeTheme->panelOnVideo);
   uiDrawNineSlice(UI_TEX_RING_R20_W3, x, y, panelW, SKIP_H, SKIP_RING_SCALE, activeTheme->hintFillOnVideo);

   // the glyph is the console's own art, untinted
   int discX = x + SKIP_PAD_L, discY = y + (SKIP_H - SKIP_DISC_D) / 2;
   uiFillCircle(discX, discY, SKIP_DISC_D, activeTheme->hintFillOnVideo);
   GfxTexture glyph = getConsoleGlyph(GLYPH_TRIANGLE);
   if (glyph.offset) {
      int glyphW = glyph.h > 0 ? (glyph.w * SKIP_GLYPH_H + glyph.h / 2) / glyph.h : SKIP_GLYPH_H;
      drawGfxTexture(discX + (SKIP_DISC_D - glyphW) / 2, discY + (SKIP_DISC_D - SKIP_GLYPH_H) / 2, glyphW, SKIP_GLYPH_H,
                     glyph, 0, 0, 1, 1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   }

   int textX = x + SKIP_TEXT_DX;
   uiDrawLabelAt(&skipPromptLabel, textX, uiLabelYForBaseline(&skipPromptLabel, y + SKIP_TITLE_BASELINE));
   int subY = uiTextYForBaseline(skipDigits.font, skipDigits.size, y + SKIP_SUB_BASELINE);
   UiTextMetrics metrics = uiGetTextMetrics(skipDigits.font, skipDigits.size);
   uiFillCircleAt(textX + SKIP_SUB_DOT / 2, subY + (metrics.capTop + metrics.baseline) / 2, SKIP_SUB_DOT,
                  getSponsorCategoryColor(segment->category));
   int digitsX = textX + SKIP_SUB_DOT + SKIP_SUB_DOT_GAP;
   uiDrawDigits(&skipDigits, digitsX, subY, left, activeTheme->textSecondary);
   uiDrawLabelAt(&skipSuffixLabel, digitsX + uiGetDigitsWidth(&skipDigits, left),
                 uiLabelYForBaseline(&skipSuffixLabel, y + SKIP_SUB_BASELINE));
}

// an icon with its ink - not its em box - centred on (cx, cy)
static void drawIconInkCentered(Icon *icon, int cx, int cy, uint32_t color)
{
   if (!icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   drawIcon(icon, cx - icon->tex->w / 2 - icon->offsetX, cy - icon->tex->h / 2 - icon->offsetY, color);
}

// The volume, for as long as the meter would have shown (2.5 s after the last step): a plate on the left
// with the level as a percentage, a bar filling from the bottom, and the meter's speaker under it (muted at
// 0, low for the bottom half). The percentage is outlined fixed-width digits, so it does not twitch as it
// counts.
static void drawVolume(void)
{
   if (volumeMeter.shownUs == 0 || sys_time_get_system_time() - volumeMeter.shownUs >= VOLUME_VISIBLE_US) return;
   int level = volumeMeter.level;
   int centerX = HUD_X + VOLUME_PLATE_W / 2;
   uiFillRounded(HUD_X, VOLUME_PLATE_Y, VOLUME_PLATE_W, VOLUME_PLATE_H, UI_RADIUS_PANEL, activeTheme->panelOnVideo);

   char percent[8];
   snprintf(percent, sizeof percent, "%d%%", (level * 100 + VOLUME_METER_PILLS / 2) / VOLUME_METER_PILLS);
   uiDrawDigitsOutlined(&timeDigits, centerX - uiGetDigitsWidth(&timeDigits, percent) / 2,
                        uiTextYForCenter(timeDigits.font, timeDigits.size, VOLUME_NUMBER_BOX_Y, VOLUME_NUMBER_BOX_H),
                        percent, activeTheme->textPrimary, activeTheme->gradientBase, TEXT_OUTLINE);

   int barX = centerX - VOLUME_BAR_W / 2, barH = VOLUME_BAR_BOTTOM - VOLUME_BAR_TOP;
   int fillH = barH * level / VOLUME_METER_PILLS;
   uiFillRounded(barX, VOLUME_BAR_TOP, VOLUME_BAR_W, barH, VOLUME_BAR_W / 2, activeTheme->progressTrack);
   if (fillH > 0) uiFillRounded(barX, VOLUME_BAR_BOTTOM - fillH, VOLUME_BAR_W, fillH, VOLUME_BAR_W / 2, activeTheme->accent);

   Icon *speaker = level <= 0 ? &volumeMeter.speakerOff
                 : level * 2 <= VOLUME_METER_PILLS ? &volumeMeter.speakerLow
                 : &volumeMeter.speakerHigh;
   drawIconInkCentered(speaker, centerX, VOLUME_ICON_CY, activeTheme->textPrimary);
}

static void drawPlay(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->letterbox);   // letterbox bars read as black, not the app background

   const uint8_t *frame = NULL;
   if (state.player) {
      int w, h;
      frame = getVideoFrame(state.player, &w, &h);
      if (frame) {
         if (!state.firstFrameSeen) showControls();   // bring up the HUD as playback starts; it auto-hides as usual
         state.firstFrameSeen = 1;
         int dx, dy, dw, dh;
         getGfxLetterboxRect(w, h, &dx, &dy, &dw, &dh);
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
      }
      measureFps(frame);
      int overlayUp = state.chaptersVisible || state.descriptionVisible;   // a full-screen overlay hides the HUD noise
      int hudUp = state.stage == STAGE_PLAYING && controlsVisible() && !overlayUp;   // live too: title, chips, LIVE
      if (hudUp) drawHud();
      if (state.showStats && frame) { updateStatLabels(); drawStatsOverlay(); }   // over the HUD's gradient
      if (state.stage == STAGE_PLAYING && !overlayUp) { drawSubtitle(hudUp); drawSkipPrompt(); drawToast(); drawVolume(); }
      if (state.chaptersVisible) drawChapters();
      if (state.descriptionVisible) drawDescription();
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen)
      drawLabelAt(&statusLabel, state.screenW / 2 - statusLabel.tt.tex.w / 2, uiLabelYForCenter(&statusLabel, 0, state.screenH));
}

static void termPlay(void)
{
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
   if (state.sbThreadActive) { joinThread(state.sbWorkerTid); state.sbThreadActive = 0; }   // usually already done
   if (state.subThreadActive) { joinThread(state.subWorkerTid); state.subThreadActive = 0; }
   if (state.chThreadActive) { joinThread(state.chWorkerTid); state.chThreadActive = 0; }
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   // save the resume position on the way out; a finished video is saved as 0 so it restarts next time.
   // only once a frame has actually played: a failed open or a resume seek that never landed must not
   // clobber a good saved position with 0. storage ignores non-id keys, so typed urls no-op.
   if (state.player && state.firstFrameSeen && !state.isLive)
      setWatchedPosition(requestedInput, isVideoEnded(state.player) ? 0 : (int)getVideoPositionSeconds(state.player));
   if (state.player || state.pendingPlayer) finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = NULL; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
   freeLabel(&statusLabel);
   for (int i = 0; i < STAT_LINES; i++) freeLabel(&statLabels[i]);
   uiFreeDigits(&timeDigits);
   uiFreeDigits(&totalDigits);
   uiFreeDigits(&skipDigits);
   freeLabel(&toastLabel);
   freeLabel(&skipPromptLabel);
   freeLabel(&skipSuffixLabel);
   freeLabel(&titleLabel);
   freeLabel(&channelLabel);
   freeLabel(&metaDotLabel);
   freeLabel(&viewsLabel);
   freeLabel(&qualityLabel);
   freeLabel(&audioNameLabel);
   freeLabel(&audioCountLabel);
   freeLabel(&subtitleLabel);
   freeLabel(&subsHudLabel);
   freeLabel(&chapterNameLabel);
   freeLabel(&chapterInfoLabel);
   freeLabel(&liveLabel);
   for (int i = 0; i < SPONSOR_CATEGORY_COUNT; i++) freeLabel(&legendLabels[i]);   // unused ones are empty
   for (int i = 0; i < MAX_CHAPTERS; i++) freeLabel(&chapterLabels[i]);
   freeLabel(&descriptionHeading);
   freeLabel(&chaptersHeading);
   uiTermHints(&hudHints);
   uiTermHints(&descriptionHints);
   uiTermHints(&chapterHints);
   freeVolumeMeter(&volumeMeter);
   freeTextTexture(&descriptionTexture);
   // no closeFont: the fonts are the app's shared ones, and closing one would blank every label on every
   // screen still using it (ui-fonts.h)
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };

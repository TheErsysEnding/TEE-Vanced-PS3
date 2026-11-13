#pragma once

// subtitle-translate - (V3.0.3, GitHub issue #4) a fetched subtitle track put into another language.
//
// YouTube's own translated tracks (&tlang=) are refused to this player: a request for one without a PO token gets
// Google's "Sorry..." page (HTTP 429) from every client that is given captions at all, over a home connection as
// over a VPN, in a browser too (probed 2026-11-13) - while the track in its own language still comes. So the player
// translates the track it gets, with Google Translate, where YouTube's own translations come from.
//
// A line of speech recognition is a few words cut out of a sentence, and translated line by line it reads as
// nonsense - the words of a sentence go in another order in another language. So the lines are joined into
// sentences first, a short one together with the next; each becomes one cue over the time its words were spoken.
// The cues are then translated a few minutes at a time: the part the video is at first, then on from there.

#include "subtitles.h"

// what the subtitle worker and the UI thread tell each other while a translation runs
typedef struct {
   volatile float position;    // the playback position in seconds: the UI thread keeps it current
   volatile int   cancel;      // the UI thread wants the worker gone: another track picked, the player closing
   volatile int   published;   // the worker: the cues are laid out and the part at the position is translated
   volatile int   stopped;     // the worker: Google stopped answering - the rest shows in the original language
} SubtitleTranslation;

// On the subtitle worker, after fetchSubtitles: lays the track out as sentences in its own language, every cue
// pending (getSubtitleCueText shows none of them), then translates them into `language` (a code such as "cs")
// until all are done, control->cancel is set or Google stops answering. 0 once the part at the position was
// translated (published is set by then); negative when not even that came - the track is then empty (count 0).
int translateSubtitles(SubtitleTrack *track, const char *language, SubtitleTranslation *control);

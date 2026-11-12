#pragma once

// stream-select - picks which of a resolved video's formats to use. Shared by playback and download so a
// downloaded file is the exact stream the player would show (same PS3-decodable H.264 + AAC pair).

#include "extractor.h"   // StreamInfo / StreamFormat

// best decodable H.264/mp4 video stream: highest resolution the PS3 decoder can keep up with, and at a
// given resolution the higher frame rate. favours the adaptive (video-only) streams over the 360p muxed
// one. NULL if none is usable.
const StreamFormat *pickBestVideo(const StreamInfo *info);

// the same for playback while the TEE Decoder may be used (teeUsable): it also admits what that decoder plays,
// 1080p60 among it. Downloads keep pickBestVideo, so their files stay what cellVdec and the XMB can play.
const StreamFormat *pickBestVideoForPlayback(const StreamInfo *info, int teeUsable);

// 1 for a format cellVdec cannot keep up with (1080p above 30 fps): only worth playing on the TEE Decoder
int needsTeeDecoder(const StreamFormat *format);

// the same pick with the viewer's quality cap (settings quality=, 0 = none): the best playable format no higher
// than maxHeight, or the lowest one there is when none is that low
const StreamFormat *pickVideoForPlayback(const StreamInfo *info, int teeUsable, int maxHeight);

// the video-only formats the player can play, one per height and frame rate, the best first: the choices of the
// player's quality menu. Returns how many went into out (at most max).
int listPlayableVideos(const StreamInfo *info, int teeUsable, const StreamFormat **out, int max);

// the AAC/mp4 audio-only stream to pair with a video-only pick, preferring itag 140 and not its
// dynamic-range-compressed copy. On a video with several audio tracks the extractor has already narrowed
// formats[] to the original track's stream. NULL if none.
const StreamFormat *pickBestAudio(const StreamInfo *info);
// (V3.0.0) the Opus stream for VP9 (itag 251), NULL when there is none
const StreamFormat *pickOpusAudio(const StreamInfo *info);

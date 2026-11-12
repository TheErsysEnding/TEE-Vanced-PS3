// stream-select - format selection shared by playback and download (see stream-select.h).

#include "stream-select.h"
#include <string.h>

// what the PS3 H.264 decoder can keep up with: 1080p at <=30 fps, or 720p at <=60 fps (fewer pixels/sec
// than 1080p30). 1080p60 (itag 298/299) exceeds it and is skipped, falling to 720p60.
static int decodableVideo(const StreamFormat *format)
{
   if (format->height <= 1080 && format->fps <= 30) return 1;
   if (format->height <= 720  && format->fps <= 60) return 1;
   return 0;
}

// what the TEE Decoder (TEE's own, on the SPEs) takes on top: up to 1920 x 1080 at up to 60 fps - 1080p60 too,
// live streams' as well (V1.1 Test9) - when the coded width is a multiple of 128 (1920, 1280, 640, 256 wide; not
// 854 or 426). decode-h264.c checks the stream itself when it opens and falls back to cellVdec.
static int teeDecodableVideo(const StreamFormat *format)
{
   int codedWidth = (format->width + 15) & ~15;
   return codedWidth % 128 == 0 && format->width <= 1920 && format->height <= 1080 && format->fps <= 60;
}

// (V3.0.0) VP9 1440p on the TEE VP9 building block: WebM, above 1080p, up to 2560 x 1440, not live (live plays its
// DASH segments as mp4). It is played only when asked for - picked in the quality list, or a quality cap of 1440
// saved by such a pick - never by Auto or for a download: with all 6 SPEs on the picture it has no sound.
static int vp9Video(const StreamFormat *format)
{
   return format->isVp9 && !format->isLiveSegmented && strcmp(format->container, "webm") == 0 &&
          format->height > 1080 && format->height <= 1440 && format->width <= 2560;
}

static int isLiveInfo(const StreamInfo *info)
{
   for (int i = 0; i < info->formatCount; i++) if (info->formats[i].isLiveSegmented) return 1;
   return 0;
}

static int playableVideo(const StreamFormat *format, int teeUsable, int allowVp9)
{
   if (!format->hasVideo || format->needsCipher || !format->url[0]) return 0;
   if (format->isVp9) return allowVp9 && vp9Video(format);
   if (strcmp(format->container, "mp4") != 0) return 0;   // mp4 == avc1 for these itags
   return decodableVideo(format) || (teeUsable && teeDecodableVideo(format));
}

// the playable video with the most pixels (and frames) up to maxHeight (0 = no limit); with nothing that low,
// the lowest one there is - a cap set for a slow connection should not turn into "no video"
static const StreamFormat *pickVideoCapped(const StreamInfo *info, int teeUsable, int maxHeight)
{
   const StreamFormat *best = NULL, *lowest = NULL;
   int allowVp9 = maxHeight >= 1440 && !isLiveInfo(info);   // a cap the viewer set by picking 1440p
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!playableVideo(format, teeUsable, allowVp9)) continue;
      if (!lowest || format->height < lowest->height) lowest = format;
      if (maxHeight && format->height > maxHeight) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best ? best : lowest;
}

int listPlayableVideos(const StreamInfo *info, int teeUsable, const StreamFormat **out, int max)
{
   int count = 0;
   int allowVp9 = !isLiveInfo(info);
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!playableVideo(format, teeUsable, allowVp9) || format->hasAudio) continue;   // the video-only streams
      int duplicate = 0;
      for (int j = 0; j < count; j++)
         if (out[j]->height == format->height && out[j]->fps == format->fps) { duplicate = 1; break; }
      if (duplicate) continue;
      // keep the list sorted, the most pixels (then frames) first
      int at = count < max ? count : max - 1;
      if (count == max && (format->height < out[at]->height ||
                           (format->height == out[at]->height && format->fps <= out[at]->fps))) continue;
      while (at > 0 && (out[at - 1]->height < format->height ||
                        (out[at - 1]->height == format->height && out[at - 1]->fps < format->fps))) {
         out[at] = out[at - 1];
         at--;
      }
      out[at] = format;
      if (count < max) count++;
   }
   return count;
}

static const StreamFormat *pickVideo(const StreamInfo *info, int teeUsable)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!playableVideo(format, teeUsable, 0)) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best;
}

const StreamFormat *pickBestVideo(const StreamInfo *info) { return pickVideo(info, 0); }

int needsTeeDecoder(const StreamFormat *format) { return format && !decodableVideo(format); }

const StreamFormat *pickBestVideoForPlayback(const StreamInfo *info, int teeUsable) { return pickVideo(info, teeUsable); }

const StreamFormat *pickVideoForPlayback(const StreamInfo *info, int teeUsable, int maxHeight)
{
   return pickVideoCapped(info, teeUsable, maxHeight);
}

// (V3.0.0) the Opus stream (itag 251) for a VP9 video, NULL when the video has none
const StreamFormat *pickOpusAudio(const StreamInfo *info)
{
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *f = &info->formats[i];
      if (f->isOpus && f->hasAudio && !f->hasVideo && !f->needsCipher && f->url[0]) return f;
   }
   return NULL;
}

const StreamFormat *pickBestAudio(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   int bestScore = -1;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasAudio || format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      // itag 140 first, and YouTube's normal mix over the dynamic-range-compressed copy of it. This used to
      // be "the last itag 140 wins", which on a video with several audio tracks meant whichever language
      // happened to come last - often one of YouTube's AI dubs. The extractor now leaves only the original's
      // stream in formats[] for such a video (see StreamInfo.audioTracks).
      int score = (format->itag == 140 ? 2 : 0) + (format->isDrc ? 0 : 1);
      if (score > bestScore) { best = format; bestScore = score; }
   }
   return best;
}

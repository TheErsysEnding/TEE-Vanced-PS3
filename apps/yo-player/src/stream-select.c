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

const StreamFormat *pickBestVideo(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;   // mp4 == avc1 for these itags
      if (!decodableVideo(format)) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best;
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

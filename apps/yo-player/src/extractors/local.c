// local - the app's own downloads as a source (V3.0.0): "local:<videoId>" opens what the Downloads page lists,
// from the files on disk - the video stream as YouTube served it (VP9 WebM or H.264 MP4) and its AAC audio beside
// it - so the player plays them like a video from YouTube, on the TEE Decoder or TEE VP9.

#include "extractor.h"
#include "downloads.h"          // readDownloadInfo
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>

static int matches(const char *input)
{
   return input && strncmp(input, LOCAL_INPUT_PREFIX, strlen(LOCAL_INPUT_PREFIX)) == 0;
}

static int extract(const char *input, StreamInfo *out)
{
   memset(out, 0, sizeof *out);
   DownloadInfo info;
   if (readDownloadInfo(input + strlen(LOCAL_INPUT_PREFIX), &info) != 0) return -1;
   strCopy(out->title, sizeof out->title, info.title);
   out->durationSeconds = durationToSeconds(info.duration);

   StreamFormat *video = &out->formats[out->formatCount++];
   video->itag = info.itag;
   video->width = info.width;
   video->height = info.height;
   video->fps = info.fps;
   video->hasVideo = 1;
   video->isVp9 = info.isVp9;
   strCopy(video->container, sizeof video->container, info.isVp9 ? "webm" : "mp4");
   snprintf(video->url, sizeof video->url, "%s", info.videoPath);

   if (info.audioPath[0]) {
      StreamFormat *audio = &out->formats[out->formatCount++];
      size_t n = strlen(info.audioPath);
      int opus = n > 5 && strcmp(info.audioPath + n - 5, ".webm") == 0;   // (V3.0.0) VP9 downloads keep their Opus
      audio->itag = opus ? 251 : 140;
      audio->hasAudio = 1;
      audio->isOpus = opus;
      strCopy(audio->container, sizeof audio->container, opus ? "webm" : "mp4");
      snprintf(audio->url, sizeof audio->url, "%s", info.audioPath);
   }
   return 0;
}

const Extractor localExtractor = { "local", matches, extract };

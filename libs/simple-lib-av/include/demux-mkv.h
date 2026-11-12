#pragma once

// demux-mkv - pulls H.264 video access units and AAC audio frames out of a Matroska (MKV) container.
// (TEE Vanced: also VP9 video tracks, as YouTube serves 1440p in WebM. Their blocks go out unchanged - the
// TEE VP9 building block takes a block's payload as it is, superframes included.)
// Reads the track metadata at open (video track number, avcC -> SPS/PPS, level, dimensions, audio
// rate/channels, timecode scale, seek index), then walks the clusters block by block: video blocks
// become Annex-B access units ready for cellVdec, audio blocks are unlaced and queued for the audio
// decode thread. Seeking jumps to the cued keyframe at or before the target.

#include "video-source.h"
#include "demux-types.h"

#define MAX_CUES 4096   // seek index entries (keyframe cue points); ~2 h at one point per 2 s

typedef struct {
   uint64_t timeNs;       // presentation time of the cued keyframe
   uint64_t clusterPos;   // absolute file offset of its cluster
} MkvCue;

typedef struct {
   VideoSource source;

   int        videoTrack;       // Matroska track number of the H.264 (or VP9) video track
   int        isVp9;            // the video track is V_VP9: blocks are handed out raw, not as Annex-B
   H264Config h264;             // SPS/PPS header + NAL length size
   int        level;            // AVC level (AVCLevelIndication), passed to cellVdec as profileLevel
   int        width, height;

   int      audioTrack;         // Matroska track number of the AAC audio track (0 = none / unsupported codec)
   int      isOpus;             // (V3.0.0) the audio track is A_OPUS (YouTube's itag 251 beside VP9)
   uint8_t  opusHead[64];       // ... its CodecPrivate ("OpusHead") for the TEE Opus building block
   int      opusHeadSize;
   uint64_t codecDelayNs;       // ... CodecDelay: a block's first sample plays at its time stamp minus this
   int      audioOnly;          // opened by openMkvAudioDemuxer: no video track, blocks read with readMkvAudioAu
   int      audioRate;          // sampling frequency in Hz
   int      audioChannels;
   AudioAuQueue audioQueue;     // demuxed audio AUs en route to the audio decode thread

   uint64_t frameDurationNs;    // nanoseconds per video frame (track DefaultDuration; 0 if absent)
   uint64_t timecodeScale;      // nanoseconds per timecode tick (default 1,000,000)
   uint64_t durationNs;         // total stream duration in nanoseconds (0 if the file omits it)
   uint64_t segmentStart;
   uint64_t segmentEnd;

   MkvCue  *cues;               // seek index from the Cues element (NULL if the file has none)
   int      cueCount;
   uint64_t cuesPos;            // absolute offset of the Cues element per SeekHead (0 = not announced)
   uint64_t chainedSeekHeadPos; // absolute offset of a second SeekHead the first one points at (0 = none)
   MkvCue  *clusterIndex;       // fallback seek index of cluster start times, grown lazily on seeks
   int      clusterIndexCount;
   uint64_t clusterIndexNextPos;   // file offset of the first cluster not yet indexed
   uint64_t pos;                // current parse position within the segment
   uint64_t clusterTimecode;    // base timecode of the cluster currently being read
   int      lacingWarned;       // logged the "laced block skipped" note once

   uint8_t *auBuffers[AU_BUFFER_COUNT];   // rotating Annex-B scratch: earlier AUs stay valid in the decoder's queue while later ones are demuxed
   int      auIndex;            // buffer the next readMkvVideoAu writes into
   int      auCapacity;         // capacity of each buffer
   uint8_t *blockBuffer;        // reused raw block-frame scratch (heap)
   int      blockCapacity;
} MkvDemuxer;

int  openMkvDemuxer(MkvDemuxer *demuxer, VideoSource *source);   // adopts an already-open source; 0 on success (video track found), -1 otherwise
int  readMkvVideoAu(MkvDemuxer *demuxer, VideoAu *au);        // 1 = got AU, 0 = end of stream, -1 = error
// (V3.0.0) an audio-only WebM (YouTube's Opus stream, itag 251): adopts the source; 0 when an Opus or AAC track is
// there. Its blocks come one at a time through readMkvAudioAu (1 = one, 0 = end of stream, -1 = error).
int  openMkvAudioDemuxer(MkvDemuxer *demuxer, VideoSource *source);
int  readMkvAudioAu(MkvDemuxer *demuxer, AudioAu *au);
void closeMkvDemuxer(MkvDemuxer *demuxer);

// jumps the cluster walk to the last cued keyframe at or before targetNs (files without Cues fall
// back to a lazily-built index of cluster start times) and clears the audio queue (only call with
// the audio consumer parked). Returns the time actually landed on. landAfter is accepted for a uniform
// seek signature but ignored (MKV always lands at/before; the next-keyframe skip is an mp4/DASH concern).
uint64_t seekMkvDemuxer(MkvDemuxer *demuxer, uint64_t targetNs, int landAfter);

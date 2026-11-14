// subtitle-translate - put a fetched subtitle track into another language (see subtitle-translate.h).

#include "subtitle-translate.h"
#include "http.h"
#include "thread.h"             // sleepMs
#include "text-sanitize.h"      // flattenWhitespace
#include "string-utilities.h"   // strCopy, truncateUtf8
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// A sentence cue holds at most this many bytes in the track's own language, so a translation a third longer still
// fits SUBTITLE_TEXT_MAX - two lines on screen. Shorter sentences are joined up to it, a longer one is cut at a
// comma (or where one of the track's own lines ends).
#define SENTENCE_MAX         110
#define SENTENCE_MAX_SECONDS 7.0f   // nor does a cue gather sentences for longer than this
#define SILENCE_SECONDS      2.0f   // a pause this long between two lines ends a sentence, full stop or not
#define LINGER_SECONDS       1.0f   // a cue stays this long after its last word, unless the next one comes first
#define MIN_SECONDS          1.5f   // and is up at least this long
// One request: the first about a minute of speech, so the cue on screen comes quickly, then a few minutes at a
// time (Google Translate takes 5000 characters a request)
#define FIRST_CHUNK_BYTES    1200
#define CHUNK_BYTES          4000
#define CHUNK_CUES           120
#define BODY_CAP             (3 * CHUNK_BYTES + 64)   // percent-encoded: at most three bytes a byte
#define RESP_CAP             (128 * 1024)             // the reply repeats every source sentence beside its translation
#define TEXT_CAP             (16 * 1024)
#define REQUEST_GAP_MS       200
#define TRANSLATE_CANCELLED  (-2)

static const char *BROWSER_UA =
   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

// ---- the track's lines as sentences ----

// The track's lines as one text, a space after each: where each line starts in it, and when it was spoken.
typedef struct {
   int    count;
   char  *text;
   int   *offset;    // count + 1 of them: line i is text[offset[i], offset[i + 1]), its space included
   float *start;
   float *spoken;    // when line i's last word is out: its own end, or the next line's start if that comes first
} Lines;

static void freeLines(Lines *lines)
{
   free(lines->text);
   free(lines->offset);
   free(lines->start);
   free(lines->spoken);
   memset(lines, 0, sizeof *lines);
}

static int copyLines(const SubtitleTrack *track, Lines *lines)
{
   memset(lines, 0, sizeof *lines);
   int count = track->count, total = 0;
   for (int i = 0; i < count; i++) total += (int)strlen(track->cues[i].text) + 1;
   lines->text   = malloc(total + 1);
   lines->offset = malloc((count + 1) * sizeof *lines->offset);
   lines->start  = malloc(count * sizeof *lines->start);
   lines->spoken = malloc(count * sizeof *lines->spoken);
   if (!lines->text || !lines->offset || !lines->start || !lines->spoken) { freeLines(lines); return -1; }

   int at = 0;
   for (int i = 0; i < count; i++) {
      const SubtitleCue *cue = &track->cues[i];
      int length = (int)strlen(cue->text);
      lines->offset[i] = at;
      memcpy(lines->text + at, cue->text, length);
      at += length;
      lines->text[at++] = ' ';
      lines->start[i] = cue->start;
      // speech recognition rolls up: a line stays while the next one is spoken, so the next start ends this one
      float spoken = cue->end;
      if (i + 1 < count && track->cues[i + 1].start > cue->start && track->cues[i + 1].start < spoken)
         spoken = track->cues[i + 1].start;
      if (spoken < cue->start + 0.3f) spoken = cue->start + 0.3f;
      lines->spoken[i] = spoken;
   }
   lines->offset[count] = at;
   lines->text[at] = 0;
   lines->count = count;
   return 0;
}

static int totalLength(const Lines *lines) { return lines->offset[lines->count]; }

// the line the byte at `pos` is in
static int lineAt(const Lines *lines, int pos)
{
   int low = 0, high = lines->count - 1;
   while (low < high) {
      int middle = (low + high + 1) / 2;
      if (lines->offset[middle] <= pos) low = middle;
      else high = middle - 1;
   }
   return low;
}

// when the text reached `pos` in line `line`: its words spread evenly over the time the line was spoken
static float timeIn(const Lines *lines, int line, int pos)
{
   int length = lines->offset[line + 1] - lines->offset[line];
   float part = length > 0 ? (float)(pos - lines->offset[line]) / (float)length : 0.0f;
   if (part < 0.0f) part = 0.0f;
   if (part > 1.0f) part = 1.0f;
   return lines->start[line] + part * (lines->spoken[line] - lines->start[line]);
}

static float startTime(const Lines *lines, int pos) { return timeIn(lines, lineAt(lines, pos), pos); }

// when the text before `end` was out: in the line its last byte is in
static float endTime(const Lines *lines, int end) { return end <= 0 ? 0.0f : timeIn(lines, lineAt(lines, end - 1), end); }

static int matches(const char *text, int pos, int total, const char *bytes)
{
   int length = (int)strlen(bytes);
   return pos + length <= total && memcmp(text + pos, bytes, length) == 0;
}

// Where the sentence running on from `from` ends: after its full stop, question or exclamation mark (and a closing
// quote or bracket) followed by a space, after a Japanese full stop, or at the end of a line a pause of
// SILENCE_SECONDS follows - *hard is then set: nothing joins over a pause. The end of the text is hard too.
static int sentenceEnd(const Lines *lines, int from, int *hard)
{
   const char *text = lines->text;
   int total = totalLength(lines);
   int line = lineAt(lines, from);
   *hard = 0;
   for (int pos = from; pos < total; pos++) {
      while (line + 1 < lines->count && pos >= lines->offset[line + 1]) line++;
      if (pos == lines->offset[line + 1] - 1 && line + 1 < lines->count &&   // the space after a line
          lines->start[line + 1] - lines->spoken[line] >= SILENCE_SECONDS) {
         *hard = 1;
         return pos;
      }
      char c = text[pos];
      if (c == '.' || c == '!' || c == '?') {
         int after = pos + 1;
         while (after < total && (text[after] == '"' || text[after] == '\'' || text[after] == ')' || text[after] == ']'))
            after++;
         if (after >= total || text[after] == ' ') return after;
      } else if (matches(text, pos, total, "\xE2\x80\xA6")) {                                   // ...
         if (pos + 3 >= total || text[pos + 3] == ' ') return pos + 3;
      } else if (matches(text, pos, total, "\xE3\x80\x82") || matches(text, pos, total, "\xEF\xBC\x81") ||
                 matches(text, pos, total, "\xEF\xBC\x9F")) {                                   // Japanese . ! ?
         return pos + 3;
      }
   }
   *hard = 1;
   return total;
}

// A sentence longer than SENTENCE_MAX is cut in the second half of that length: after its last comma, semicolon,
// colon or dash there, else where one of the track's lines starts, else at the last space, else (Japanese, no
// spaces) at a character's start.
static int cutLongSentence(const Lines *lines, int begin)
{
   const char *text = lines->text;
   int total = totalLength(lines);
   int limit = begin + SENTENCE_MAX < total ? begin + SENTENCE_MAX : total;
   int low = begin + SENTENCE_MAX / 2;
   for (int pos = limit - 1; pos > low; pos--) {
      if (text[pos] == ' ' && (text[pos - 1] == ',' || text[pos - 1] == ';' || text[pos - 1] == ':' ||
                               (text[pos - 1] == '-' && text[pos - 2] == ' ')))
         return pos;
      if (matches(text, pos - 3, total, "\xE3\x80\x81") || matches(text, pos - 3, total, "\xEF\xBC\x8C"))   // Japanese ,
         return pos;
   }
   int line = lineAt(lines, limit - 1);
   if (line > 0 && lines->offset[line] - 1 > low) return lines->offset[line] - 1;   // the space before that line
   for (int pos = limit - 1; pos > low; pos--)
      if (text[pos] == ' ') return pos;
   int pos = limit;
   while (pos > begin + 1 && ((unsigned char)text[pos] & 0xC0) == 0x80) pos--;
   return pos;
}

// Lays the lines out as sentence cues in track->cues, every one pending, and returns how many there are.
static int layOutSentences(const Lines *lines, SubtitleTrack *track)
{
   const char *text = lines->text;
   int total = totalLength(lines);
   int count = 0, pos = 0;
   while (pos < total && count < MAX_SUBTITLE_CUES) {
      while (pos < total && text[pos] == ' ') pos++;
      if (pos >= total) break;
      int begin = pos, end = pos;
      float startAt = startTime(lines, begin);
      for (;;) {
         int hard;
         int next = sentenceEnd(lines, end, &hard);
         if (end == begin && next - begin > SENTENCE_MAX) { end = cutLongSentence(lines, begin); break; }
         if (end > begin && (next - begin > SENTENCE_MAX || endTime(lines, next) - startAt > SENTENCE_MAX_SECONDS))
            break;
         end = next;
         if (hard || end >= total) break;
      }
      pos = end;

      int length = end - begin;
      while (length > 0 && text[begin + length - 1] == ' ') length--;
      if (length <= 0) continue;
      if (length > SUBTITLE_TEXT_MAX - 1) {   // past SENTENCE_MAX it cannot be, but never cut a character in half
         length = SUBTITLE_TEXT_MAX - 1;
         while (length > 0 && ((unsigned char)text[begin + length] & 0xC0) == 0x80) length--;
      }
      SubtitleCue *cue = &track->cues[count++];
      memcpy(cue->text, text + begin, length);
      cue->text[length] = 0;
      cue->start = startAt;
      cue->end = endTime(lines, begin + length) + LINGER_SECONDS;
      if (cue->end < cue->start + MIN_SECONDS) cue->end = cue->start + MIN_SECONDS;
      cue->pending = 1;
   }
   return count;
}

// ---- Google Translate ----

// waits `ms`, or less when the player cancels the translation meanwhile (another track, the player closing)
static void waitUnlessCancelled(const SubtitleTranslation *control, int ms)
{
   for (int waited = 0; waited < ms && !control->cancel; waited += 100) sleepMs(100);
}

typedef struct {
   SubtitleTrack       *track;
   SubtitleTranslation *control;
   char  url[160];
   char *body;
   char *resp;
   char *text;                     // the reply's translations, then cut into lines
   char *lines[CHUNK_CUES + 8];
   int   lineCount;                // may be more than the array holds: that is a mismatch all the same
   int   requests;
} Job;

static int appendEncoded(char *out, int cap, int at, const char *text)
{
   static const char HEX[] = "0123456789ABCDEF";
   for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
      if (at + 3 >= cap) return -1;
      if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
          *c == '-' || *c == '_' || *c == '.' || *c == '~') {
         out[at++] = (char)*c;
      } else {
         out[at++] = '%';
         out[at++] = HEX[*c >> 4];
         out[at++] = HEX[*c & 15];
      }
   }
   out[at] = 0;
   return at;
}

static int hexDigits(const char *p)
{
   int value = 0;
   for (int i = 0; i < 4; i++) {
      char c = p[i];
      int digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else return -1;
      value = value * 16 + digit;
   }
   return value;
}

static void putCodePoint(char *out, int cap, int *at, unsigned point)
{
   if (point < 0x80) {
      if (*at + 1 < cap) out[(*at)++] = (char)point;
   } else if (point < 0x800) {
      if (*at + 2 < cap) { out[(*at)++] = (char)(0xC0 | (point >> 6)); out[(*at)++] = (char)(0x80 | (point & 0x3F)); }
   } else if (point < 0x10000) {
      if (*at + 3 < cap) {
         out[(*at)++] = (char)(0xE0 | (point >> 12));
         out[(*at)++] = (char)(0x80 | ((point >> 6) & 0x3F));
         out[(*at)++] = (char)(0x80 | (point & 0x3F));
      }
   } else if (*at + 4 < cap) {
      out[(*at)++] = (char)(0xF0 | (point >> 18));
      out[(*at)++] = (char)(0x80 | ((point >> 12) & 0x3F));
      out[(*at)++] = (char)(0x80 | ((point >> 6) & 0x3F));
      out[(*at)++] = (char)(0x80 | (point & 0x3F));
   }
}

// The translations out of Google's reply, one after another into out. The reply reads
// [[["<translation>","<source>",null,...],["<translation>",...]],null,"en",...]: the first string of every array
// on the third level, until the list they are in closes. A translation ends in "\n" where its source line did.
// Returns the length, -1 for a reply of another shape.
static int collectTranslation(const char *json, int length, char *out, int cap)
{
   const char *p = json, *end = json + length;
   int depth = 0, at = 0, first = 0, found = 0;
   out[0] = 0;
   while (p < end) {
      char c = *p;
      if (c == '[' || c == '{') { depth++; first = (depth == 3); p++; continue; }
      if (c == ']' || c == '}') {
         depth--;
         first = 0;
         p++;
         if (depth == 1) { out[at] = 0; return found ? at : -1; }   // the list of sentences is closed
         if (depth <= 0) break;
         continue;
      }
      if (c != '"') {
         if (c != ' ' && c != '\n' && c != '\r' && c != '\t') first = 0;   // a comma, null or number came first
         p++;
         continue;
      }
      int take = first && depth == 3;
      first = 0;
      p++;
      while (p < end && *p != '"') {
         if (*p != '\\') {
            if (take && at + 1 < cap) out[at++] = *p;
            p++;
            continue;
         }
         if (++p >= end) break;
         char escaped = *p++;
         if (escaped == 'u') {
            int value = p + 4 <= end ? hexDigits(p) : -1;
            if (value < 0) continue;
            p += 4;
            unsigned point = (unsigned)value;
            if (point >= 0xD800 && point <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
               int lowHalf = hexDigits(p + 2);
               if (lowHalf >= 0xDC00 && lowHalf <= 0xDFFF) {
                  point = 0x10000 + ((point - 0xD800) << 10) + (unsigned)(lowHalf - 0xDC00);
                  p += 6;
               }
            }
            if (take) putCodePoint(out, cap, &at, point);
         } else if (take && at + 1 < cap) {
            out[at++] = escaped == 'n' ? '\n' : (escaped == 't' || escaped == 'r' || escaped == 'b' || escaped == 'f') ? ' '
                                                                                                                   : escaped;
         }
      }
      p++;   // the closing quote
      if (take) found = 1;
   }
   return -1;
}

// cuts job->text into its lines (job->lines, lineCount); a newline at its very end leaves no empty line behind
static void splitLines(Job *job)
{
   int room = (int)(sizeof job->lines / sizeof *job->lines);
   job->lineCount = 0;
   char *line = job->text;
   for (;;) {
      char *newline = strchr(line, '\n');
      if (newline) *newline = 0;
      if (newline || line[0] || job->lineCount == 0) {
         if (job->lineCount < room) job->lines[job->lineCount] = line;
         job->lineCount++;
      }
      if (!newline) break;
      line = newline + 1;
   }
}

// Asks Google Translate for cues [from, to), one a line; job->lines holds the answer's lines then. A refusal is not
// asked again (HTTP 429 says "not now", for longer than a retry would wait), a lost connection twice more.
static int requestTranslation(Job *job, int from, int to)
{
   memcpy(job->body, "q=", 3);
   int at = 2;
   for (int i = from; i < to && at >= 0; i++) {
      if (i > from) at = appendEncoded(job->body, BODY_CAP, at, "\n");
      if (at >= 0) at = appendEncoded(job->body, BODY_CAP, at, job->track->cues[i].text);
   }
   if (at < 0) return -1;   // more than a chunk can be: cannot happen

   const HttpHeader headers[] = {
      { "Content-Type", "application/x-www-form-urlencoded;charset=UTF-8" },
      { "User-Agent", BROWSER_UA },
   };
   int status = 0;
   for (int attempt = 0; attempt < 3; attempt++) {
      if (attempt) waitUnlessCancelled(job->control, attempt == 1 ? 1000 : 4000);
      if (job->control->cancel) return TRANSLATE_CANCELLED;
      int replyLength = 0;
      status = 0;
      int rc = fetchHttp("POST", job->url, headers, 2, job->body, at, job->resp, RESP_CAP, &replyLength, &status);
      job->requests++;
      if (rc == 0 && status == 200) {
         if (collectTranslation(job->resp, replyLength, job->text, TEXT_CAP) < 0) {
            logWarn("[subs] translation: a reply of another shape (%d bytes)\n", replyLength);
            return -1;
         }
         splitLines(job);
         return 0;
      }
      logWarn("[subs] translation: rc=%d status=%d (try %d)\n", rc, status, attempt + 1);
      if (status == 429 || status == 403 || status == 400) break;
   }
   return status > 0 ? -status : -1;
}

// a cue's translation in place of its text, and the cue shown from then on; an empty answer keeps the text
static void putTranslation(SubtitleCue *cue, const char *translation)
{
   char line[SUBTITLE_TEXT_MAX * 2];
   strCopy(line, sizeof line, translation);
   flattenWhitespace(line);
   truncateUtf8(line, SUBTITLE_TEXT_MAX - 1);
   if (line[0]) strCopy(cue->text, sizeof cue->text, line);
   __sync_synchronize();   // the text before the flag: the UI thread reads a cue only once it is not pending
   cue->pending = 0;
}

// Translates cues [from, to). When Google's lines do not match the cues one to one (it joined two lines, or split
// one), each half is asked for on its own, down to a single cue, whose lines are then all its own.
static int translateRange(Job *job, int from, int to, int depth)
{
   int rc = requestTranslation(job, from, to);
   if (rc < 0) return rc;
   int cues = to - from;
   if (job->lineCount == cues) {
      for (int i = 0; i < cues; i++) putTranslation(&job->track->cues[from + i], job->lines[i]);
      return 0;
   }
   int room = (int)(sizeof job->lines / sizeof *job->lines);
   if (cues == 1) {
      char joined[SUBTITLE_TEXT_MAX * 2];
      joined[0] = 0;
      for (int i = 0; i < job->lineCount && i < room; i++) {
         if (i) strncat(joined, " ", sizeof joined - strlen(joined) - 1);
         strncat(joined, job->lines[i], sizeof joined - strlen(joined) - 1);
      }
      putTranslation(&job->track->cues[from], joined);
      return 0;
   }
   logInfo("[subs] translation: %d line(s) back for %d cue(s), asking for each half\n", job->lineCount, cues);
   if (depth >= 8) {   // cannot happen (120 cues halve to one in 7 steps); left as they were
      for (int i = from; i < to; i++) putTranslation(&job->track->cues[i], job->track->cues[i].text);
      return 0;
   }
   int middle = from + cues / 2;
   rc = translateRange(job, from, middle, depth + 1);
   return rc < 0 ? rc : translateRange(job, middle, to, depth + 1);
}

// the first pending cue that is on screen at `position` or comes after it, else the first pending one; -1 when
// none is left
static int nextPending(const SubtitleTrack *track, float position)
{
   int first = -1;
   for (int i = 0; i < track->count; i++) {
      if (!track->cues[i].pending) continue;
      if (track->cues[i].end > position) return i;
      if (first < 0) first = i;
   }
   return first;
}

// Google Translate's code for a language: the part before a dash ("pt-BR" is pt), Filipino tl, Norwegian no
static void googleLanguage(const char *code, char *out, int cap)
{
   int length = 0;
   while (code[length] && code[length] != '-' && length < cap - 1) {
      char c = code[length];
      out[length++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
   }
   out[length] = 0;
   if (strcmp(out, "fil") == 0) strCopy(out, cap, "tl");
   else if (strcmp(out, "nb") == 0) strCopy(out, cap, "no");
}

int translateSubtitles(SubtitleTrack *track, const char *language, SubtitleTranslation *control)
{
   control->published = control->stopped = 0;
   int lineCount = track->count;
   Lines lines;
   if (lineCount <= 0 || copyLines(track, &lines) < 0) { track->count = 0; return -1; }
   track->count = layOutSentences(&lines, track);
   freeLines(&lines);
   logInfo("[subs] translating into %s: %d line(s) as %d sentence(s)\n", language, lineCount, track->count);

   Job job;
   memset(&job, 0, sizeof job);
   job.track = track;
   job.control = control;
   char target[12];
   googleLanguage(language, target, sizeof target);
   snprintf(job.url, sizeof job.url,
            "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=%s&dt=t", target);
   job.body = malloc(BODY_CAP);
   job.resp = malloc(RESP_CAP);
   job.text = malloc(TEXT_CAP);
   int rc = (job.body && job.resp && job.text && track->count > 0) ? 0 : -1;

   int translated = 0;
   while (rc == 0 && !control->cancel) {
      int from = nextPending(track, control->position);
      if (from < 0) break;
      int cap = control->published ? CHUNK_BYTES : FIRST_CHUNK_BYTES;
      int to = from, bytes = 0;
      while (to < track->count && track->cues[to].pending && to - from < CHUNK_CUES) {
         int length = (int)strlen(track->cues[to].text) + 1;
         if (to > from && bytes + length > cap) break;
         bytes += length;
         to++;
      }
      rc = translateRange(&job, from, to, 0);
      if (rc < 0) break;
      translated += to - from;
      if (!control->published) {
         __sync_synchronize();   // the cues and their count before the word that they may be read
         control->published = 1;
         logInfo("[subs] translation: the first %d sentence(s) in, from %.1f s\n", to - from, track->cues[from].start);
      }
      waitUnlessCancelled(control, REQUEST_GAP_MS);
   }
   free(job.body);
   free(job.resp);
   free(job.text);

   if (rc < 0 && rc != TRANSLATE_CANCELLED) {
      if (!control->published) {
         logWarn("[subs] translation into %s failed (%d) after %d request(s)\n", language, rc, job.requests);
         track->count = 0;
         return rc;
      }
      for (int i = 0; i < track->count; i++) track->cues[i].pending = 0;   // the rest in the track's own language
      __sync_synchronize();
      control->stopped = 1;
   }
   logInfo("[subs] translated %d of %d sentence(s) into %s in %d request(s)%s\n", translated, track->count, language,
           job.requests, control->stopped ? " - Google stopped answering, the rest stays as it was" :
                         control->cancel ? " - stopped for another track" : "");
   return control->published ? 0 : (rc < 0 ? rc : -1);
}

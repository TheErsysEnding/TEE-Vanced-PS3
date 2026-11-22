// yt-text (V3.1.0) - YouTube's English meta phrases in the app's language (see yt-text.h).

#include "yt-text.h"
#include "i18n.h"
#include "string-utilities.h"   // strCopy
#include <stdio.h>
#include <string.h>

// What a count stands for, as far as the plural form goes: "23" -> 23, "6,100" -> 6100. A count YouTube shortened
// ("1.2K", "4.6M") is a large round number in every language's rules: one million.
static long countOf(const char *text, int length)
{
   long n = 0;
   for (int i = 0; i < length; i++) {
      char c = text[i];
      if (c >= '0' && c <= '9') { if (n < 100000000L) n = n * 10 + (c - '0'); }
      else if (c == 'K' || c == 'M' || c == 'B' || c == '.') return 1000000L;
   }
   return n;
}

// "<unit> ago" / "<unit>s ago" after a count: the format for it, or NULL
static const char *agoFormat(const char *rest, long n)
{
   static const struct { const char *unit; int kind; } UNITS[] = {
      { "second", 0 }, { "minute", 1 }, { "hour", 2 }, { "day", 3 }, { "week", 4 }, { "month", 5 }, { "year", 6 },
   };
   for (int u = 0; u < (int)(sizeof UNITS / sizeof UNITS[0]); u++) {
      size_t len = strlen(UNITS[u].unit);
      if (strncmp(rest, UNITS[u].unit, len) != 0) continue;
      const char *after = rest + len;
      if (*after == 's') after++;
      if (strcmp(after, " ago") != 0) return NULL;
      switch (UNITS[u].kind) {
      case 0:  return trn(n, "%s second ago", "%s seconds ago");
      case 1:  return trn(n, "%s minute ago", "%s minutes ago");
      case 2:  return trn(n, "%s hour ago", "%s hours ago");
      case 3:  return trn(n, "%s day ago", "%s days ago");
      case 4:  return trn(n, "%s week ago", "%s weeks ago");
      case 5:  return trn(n, "%s month ago", "%s months ago");
      default: return trn(n, "%s year ago", "%s years ago");
      }
   }
   return NULL;
}

// "<count> <words>": the format for the words, or NULL
static const char *countFormat(const char *rest, long n)
{
   if (strcmp(rest, "views") == 0 || strcmp(rest, "view") == 0) return trn(n, "%s view", "%s views");
   if (strcmp(rest, "watching") == 0 || strcmp(rest, "watching now") == 0) return trn(n, "%s watching", "%s watching");
   if (strcmp(rest, "subscribers") == 0 || strcmp(rest, "subscriber") == 0) return trn(n, "%s subscriber", "%s subscribers");
   if (strcmp(rest, "videos") == 0 || strcmp(rest, "video") == 0) return trn(n, "%s video", "%s videos");
   return agoFormat(rest, n);
}

// a phrase that starts with a count; 1 when it was one this knows
static int localizeCount(const char *in, char *out, int cap)
{
   int length = 0;
   while (in[length] && ((in[length] >= '0' && in[length] <= '9') || in[length] == ',' || in[length] == '.' ||
                         in[length] == 'K' || in[length] == 'M' || in[length] == 'B'))
      length++;
   if (length == 0 || length > 24 || in[length] != ' ' || in[0] < '0' || in[0] > '9') return 0;
   const char *format = countFormat(in + length + 1, countOf(in, length));
   if (!format) return 0;
   char number[32];
   memcpy(number, in, (size_t)length);
   number[length] = 0;
   snprintf(out, cap, format, number);
   return 1;
}

const char *localizeYouTubeText(const char *in, char *out, int cap)
{
   if (cap <= 0) return out;
   out[0] = 0;
   if (!in || !in[0]) return out;
   if (i18nIndex() == 0) { strCopy(out, cap, in); return out; }   // English: as YouTube wrote it
   if (localizeCount(in, out, cap)) return out;
   char inner[96];
   if (strncmp(in, "Streamed ", 9) == 0 && localizeCount(in + 9, inner, sizeof inner)) {
      snprintf(out, cap, tr("Streamed %s"), inner);
      return out;
   }
   if (strncmp(in, "Premiered ", 10) == 0 && localizeCount(in + 10, inner, sizeof inner)) {
      snprintf(out, cap, tr("Premiered %s"), inner);
      return out;
   }
   if (strcmp(in, "No views") == 0) { strCopy(out, cap, tr("No views")); return out; }
   // the Downloads page writes its two states into the same line (downloads.c): its words, not YouTube's
   if (strcmp(in, "Downloading") == 0) { strCopy(out, cap, tr("Downloading")); return out; }
   if (strcmp(in, "Waiting") == 0) { strCopy(out, cap, tr("Waiting")); return out; }
   strCopy(out, cap, in);
   return out;
}

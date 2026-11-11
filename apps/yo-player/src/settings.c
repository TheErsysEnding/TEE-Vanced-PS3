// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "settings.h"
#include "app-paths.h"
#include "settings-file.h"
#include "dbg.h"
#include <stdio.h>              // snprintf: the runtime path
#include <string.h>             // strlen

// the file written on first launch: each setting at its default, with a comment explaining the values.
static const char *DEFAULT_SETTINGS =
   "# yo-player settings - edit over FTP; changes apply on the next launch\n"
   "\n"
   "sponsorblock-mode=ads\n"
   "# off = never skip anything, ads = skip paid promotions only (sponsors, self-promo,\n"
   "# like/subscribe reminders), all = also skip intros, outros, filler and non-music sections\n"
   "\n"
   "theme=tee\n"
   "# which colour theme to start in - one of the [Name] blocks in themes.txt (lower case,\n"
   "# spaces become hyphens). the shipped one is tee; youtube-classic is the old palette.\n"
   "\n"
   "region=US\n"
   "# the country whose YouTube the lists come from: Trending and Live follow it, Search and Sports only\n"
   "# partly. a two-letter code as listed in Settings > Region, e.g. US, GB, DE, AT, CH, IT, TR, RU.\n"
   "# the app itself stays in English.\n"
   "\n"
   "video-decoder=auto\n"
   "# auto = the TEE Decoder (TEE's own H.264 decoder on the SPEs: 1080p60) where it can decode the video,\n"
   "# Sony's cellVdec otherwise; sony = cellVdec only (1080p at up to 30 fps, 720p at 60)\n"
   "\n"
   "tee-decoder-spes=4\n"
   "# 4 = 1 CABAC + 3 pixel SPEs, 5 = 1 CABAC + 4 pixel (more headroom; with the sound all 6 SPEs are busy)\n"
   "\n"
   "vp9-spes=6\n"
   "# 1440p (VP9, picked in START > Quality): 6 = 2 entropy + 4 pixel SPEs, 60 fps but no sound (Sony's sound\n"
   "# decoder needs an SPE of its own); 5 = 1 entropy + 4 pixel with sound, slower (50 - 63 fps measured)\n"
   "\n"
   "quality=auto\n"
   "# the highest video quality to play: auto = the best the console plays smoothly, or a height such as 1080,\n"
   "# 720, 480, 360 or 144 - lower saves bandwidth on a slow connection. The player sets it too: START > Quality.\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;
static VideoDecoderSetting videoDecoder = VIDEO_DECODER_AUTO;
static int teeDecoderSpes = 4;
static int vp9Spes = 6;
static int downloadQuality = 0;   // download-quality=: the R3 page's last choice (downloads.h DownloadQuality)
static int qualityCap = 0;   // quality=: the highest video height to play, 0 = auto

// ---- content region ----

// The countries the lists can come from, in the picker's order: the German-speaking three first, then A-Z by
// English name. Only these codes are ever sent - YouTube answers an unknown or lower-case gl with HTTP 200 and
// an empty page, not an error - and each one was probed in November 2026: its Trending lists and Live chips came
// back filled, apart from Live News in some (youtube.c falls back to a search there). Left out: India, where
// many titles are in scripts neither font of the console can draw, and China, which YouTube does not take. South
// Korea stays only while its Hangul titles prove legible on the console; deleting its line is the whole change -
// nothing else refers to a position in this table.
static const ContentRegion REGIONS[] = {
   { "DE", "Germany" },
   { "AT", "Austria" },
   { "CH", "Switzerland" },
   { "AU", "Australia" },
   { "BE", "Belgium" },
   { "BR", "Brazil" },
   { "BG", "Bulgaria" },
   { "CA", "Canada" },
   { "HR", "Croatia" },
   { "CZ", "Czechia" },
   { "DK", "Denmark" },
   { "FI", "Finland" },
   { "FR", "France" },
   { "GR", "Greece" },
   { "HU", "Hungary" },
   { "IE", "Ireland" },
   { "IT", "Italy" },
   { "JP", "Japan" },
   { "MX", "Mexico" },
   { "NL", "Netherlands" },
   { "NO", "Norway" },
   { "PL", "Poland" },
   { "PT", "Portugal" },
   { "RO", "Romania" },
   { "RU", "Russia" },
   { "RS", "Serbia" },
   { "KR", "South Korea" },
   { "ES", "Spain" },
   { "SE", "Sweden" },
   { "TR", "Turkey" },
   { "UA", "Ukraine" },
   { "GB", "United Kingdom" },
   { "US", "United States" },   // the default: what every earlier build sent
};
#define REGION_COUNT ((int)(sizeof REGIONS / sizeof REGIONS[0]))

// The region in use, as an index into REGIONS; -1 until loadSettings has run, which reads as the default. Written
// only by the UI thread (loadSettings, setContentRegionIndex) and read by the list workers: an aligned int is never
// torn, volatile makes every read a real load, and each request reads it once, so a request started before a
// change simply finishes with the old region. A mutable string here would need a lock (thread.h).
static volatile int regionIndex = -1;
static unsigned     regionRevision;
static int          regionUnsaved;   // the region in use is not what settings.txt says (its save failed)

// The default, looked up by code so that reordering the table - or removing a line from it - cannot move it.
static int defaultRegionIndex(void)
{
   for (int i = 0; i < REGION_COUNT; i++) if (strEq(REGIONS[i].code, "US")) return i;
   return 0;   // unreachable while the table lists the United States
}

int getContentRegionCount(void) { return REGION_COUNT; }

const ContentRegion *getContentRegionAt(int index)
{
   return (index >= 0 && index < REGION_COUNT) ? &REGIONS[index] : NULL;
}

int getContentRegionIndex(void)
{
   int index = regionIndex;   // one read: a worker may ask while the UI thread changes it
   return (index >= 0 && index < REGION_COUNT) ? index : defaultRegionIndex();
}

const char *getContentRegionCode(void) { return REGIONS[getContentRegionIndex()].code; }

unsigned getContentRegionRevision(void) { return regionRevision; }

// region=<code>: its first two characters in either case, followed by nothing but the line's end, blanks or a
// '#' comment. The table index, or -1.
static int parseRegion(const char *value)
{
   for (int i = 0; i < REGION_COUNT; i++) {
      const char *code = REGIONS[i].code;
      if (toUpperChar(value[0]) != code[0] || toUpperChar(value[1]) != code[1]) continue;
      char after = value[2];
      if (after == 0 || after == '\r' || after == '\n' || after == ' ' || after == '\t' || after == '#') return i;
   }
   return -1;
}

// ---- the file ----

// the directory migrateAppData settled on (see app-paths.h; main runs it first). Built from YO_DATA_DIR the path
// was /dev_hdd0/yo-player even on a console that refuses that folder: the file was never created there, every
// launch ran on the defaults, and theme.c read its theme= line from the data directory's settings.txt - a
// different file from the one this module looked for.
static void settingsPath(char *out, int cap)
{
   snprintf(out, cap, "%s/settings.txt", getAppDataDir());
}

void loadSettings(void)
{
   char path[128];
   settingsPath(path, sizeof path);

   // The whole file, not its first 2 KB: the app appends region= at the end of a file that has none, and a
   // shorter buffer would lose that line on the next start. A file that exists but cannot be read is left alone:
   // the library's loader would write the defaults over it, and with them over the region and every hand edit.
   char text[SETTINGS_FILE_CAP];
   int length = readFile(path, text, sizeof text);
   if (length < 0 && fileExists(path)) {
      logWarn("[settings] %s cannot be read: running on the defaults, the file is left as it is\n", path);
      strCopy(text, sizeof text, DEFAULT_SETTINGS);
   } else if (length <= 0) {   // missing or empty: write the documented defaults
      strCopy(text, sizeof text, DEFAULT_SETTINGS);
      if (writeFile(path, DEFAULT_SETTINGS, (uint64_t)getStrLen(DEFAULT_SETTINGS)) == 0)
         logInfo("[settings] created %s with defaults\n", path);
      else
         logWarn("[settings] could not create %s; running on the defaults\n", path);
   } else if (length >= (int)sizeof text - 1) {
      logWarn("[settings] %s is larger than %d bytes: only its start is read\n", path, (int)sizeof text - 1);
   }

   const char *mode = findSettingValue(text, "sponsorblock-mode");
   if (mode) {
      if      (settingValueEquals(mode, "off")) sponsorblockMode = SPONSORBLOCK_OFF;
      else if (settingValueEquals(mode, "ads")) sponsorblockMode = SPONSORBLOCK_ADS;
      else if (settingValueEquals(mode, "all")) sponsorblockMode = SPONSORBLOCK_ALL;
      else logWarn("[settings] unknown sponsorblock-mode value, using ads\n");
   }

   const char *decoder = findSettingValue(text, "video-decoder");
   if (decoder) {
      if      (settingValueEquals(decoder, "auto")) videoDecoder = VIDEO_DECODER_AUTO;
      else if (settingValueEquals(decoder, "sony")) videoDecoder = VIDEO_DECODER_SONY;
      else logWarn("[settings] unknown video-decoder value, using auto\n");
   }
   const char *spes = findSettingValue(text, "tee-decoder-spes");
   if (spes) {
      if      (settingValueEquals(spes, "4")) teeDecoderSpes = 4;
      else if (settingValueEquals(spes, "5")) teeDecoderSpes = 5;
      else logWarn("[settings] tee-decoder-spes takes 4 or 5, using 4\n");
   }
   const char *dlq = findSettingValue(text, "download-quality");
   if (dlq) {
      if      (settingValueEquals(dlq, "1440")) downloadQuality = 0;
      else if (settingValueEquals(dlq, "1080")) downloadQuality = 1;
      else if (settingValueEquals(dlq, "xmb"))  downloadQuality = 2;
   }
   const char *vp9 = findSettingValue(text, "vp9-spes");
   if (vp9) {
      if      (settingValueEquals(vp9, "6")) vp9Spes = 6;
      else if (settingValueEquals(vp9, "5")) vp9Spes = 5;
      else logWarn("[settings] vp9-spes takes 5 or 6, using 6\n");
   }
   logInfo("[settings] video-decoder %s, tee-decoder-spes %d, vp9-spes %d\n", videoDecoder == VIDEO_DECODER_SONY ? "sony" : "auto",
           teeDecoderSpes, vp9Spes);
   const char *quality = findSettingValue(text, "quality");
   if (quality && !settingValueEquals(quality, "auto")) {
      int height = 0;
      for (const char *digit = quality; *digit >= '0' && *digit <= '9' && height < 100000; digit++)
         height = height * 10 + (*digit - '0');
      if (height >= 100 && height <= 4320) qualityCap = height;
      else logWarn("[settings] quality takes auto or a height such as 720, using auto\n");
   }
   if (qualityCap) logInfo("[settings] quality up to %dp\n", qualityCap);

   const char *region = findSettingValue(text, "region");
   int index = region ? parseRegion(region) : -1;
   regionIndex = index >= 0 ? index : defaultRegionIndex();
   const ContentRegion *inUse = &REGIONS[getContentRegionIndex()];
   if (index >= 0) {
      logInfo("[settings] region %s (%s)\n", inUse->code, inUse->name);
   } else if (region) {
      int shown = 0;   // the value up to its line's end, so the log never runs on into the next line
      while (shown < 24 && region[shown] && region[shown] != '\r' && region[shown] != '\n') shown++;
      logWarn("[settings] unknown region \"%.*s\", using %s\n", shown, region, inUse->code);
   } else {
      logInfo("[settings] region %s (default)\n", inUse->code);
   }
}

// Puts region=<code> into settings.txt and keeps every other line, comments and blank lines included. The
// library's rewrite treats a file it cannot read as empty, and stops reading at 4 KB without noticing: a passing
// read error would leave only the region= line behind, a long hand-edited file would lose its tail. So the file is
// read whole here first, nothing is written unless that worked, and the new text is built from what was read -
// no second read that could fail in between. A missing or empty file becomes the documented defaults with the
// chosen region, as loadSettings would write it. Main thread only: 8 KB of buffers on the stack.
// puts key=value into settings.txt and keeps every other line (see the comment above for why it reads first)
static int saveSetting(const char *key, const char *value)
{
   char path[128];
   settingsPath(path, sizeof path);

   char text[SETTINGS_FILE_CAP];
   int length = readFile(path, text, sizeof text);
   if (length < 0 && fileExists(path)) {
      logError("[settings] %s not saved: %s exists but cannot be read\n", key, path);
      return -1;
   }
   if (length >= SETTINGS_FILE_CAP - 64) {   // room for the line, and proof the read was not cut short
      logError("[settings] %s not saved: %s is too large to rewrite whole (%d bytes)\n", key, path, length);
      return -1;
   }
   if (length <= 0) strCopy(text, sizeof text, DEFAULT_SETTINGS);

   int keyLength = (int)strlen(key);
   char out[SETTINGS_FILE_CAP];
   int written = 0, found = 0;
   for (const char *line = text; *line; ) {   // as rewriteSettingKey does, but on the text already read
      const char *lineEnd = line;
      while (*lineEnd && *lineEnd != '\n') lineEnd++;
      if (lineDeclaresKey(line, key, keyLength)) {
         appendSettingLine(out, sizeof out, &written, key, value);
         found = 1;
      } else {
         for (const char *c = line; c < lineEnd && written < (int)sizeof out - 1; c++) out[written++] = *c;
         if (written < (int)sizeof out - 1) out[written++] = '\n';
      }
      line = *lineEnd ? lineEnd + 1 : lineEnd;
   }
   if (!found) appendSettingLine(out, sizeof out, &written, key, value);

   if (writeFile(path, out, (uint64_t)written) != 0) {
      logError("[settings] %s not saved: writing %s failed\n", key, path);
      return -1;
   }
   return 0;
}

static int saveRegion(const char *code) { return saveSetting("region", code); }

int getQualityCap(void) { return qualityCap; }

int setQualityCap(int height)
{
   if (height < 0) height = 0;
   qualityCap = height;
   char value[16];
   if (height) snprintf(value, sizeof value, "%d", height);
   else        strCopy(value, sizeof value, "auto");
   int rc = saveSetting("quality", value);
   logInfo("[settings] quality %s (%s)\n", value, rc == 0 ? "saved" : "NOT saved");
   return rc;
}

int setContentRegionIndex(int index)
{
   if (index < 0 || index >= REGION_COUNT) {
      logError("[settings] region index %d is not in the table\n", index);
      return -1;
   }
   int old = getContentRegionIndex();
   if (index == old && !regionUnsaved) return 0;   // nothing changed, nothing to write

   if (index != old) {
      regionIndex = index;   // in use from the next request, saved or not
      regionRevision++;
   }
   int rc = saveRegion(REGIONS[index].code);
   regionUnsaved = rc != 0;
   logInfo("[settings] region %s -> %s (%s)\n", REGIONS[old].code, REGIONS[index].code, rc == 0 ? "saved" : "NOT saved");
   return rc;
}

SponsorblockMode getSponsorblockMode(void) { return sponsorblockMode; }
VideoDecoderSetting getVideoDecoderSetting(void) { return videoDecoder; }
int getTeeDecoderSpes(void) { return teeDecoderSpes; }
int getVp9Spes(void) { return vp9Spes; }

int getDownloadQuality(void) { return downloadQuality; }

int setDownloadQuality(int quality)
{
   static const char *values[3] = { "1440", "1080", "xmb" };
   if (quality < 0 || quality > 2) return -1;
   if (quality == downloadQuality) return 0;
   downloadQuality = quality;
   return saveSetting("download-quality", values[quality]);
}

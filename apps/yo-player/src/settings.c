// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "settings.h"
#include "app-paths.h"
#include "settings-file.h"
#include "dbg.h"
#include <stdio.h>              // snprintf: the runtime path
#include <string.h>             // strlen

// (V3.0.3) subtitle-language= and what it means, at the end of a new file. Also what an old file's block becomes
// (migrateSubtitleSetting): V3.0.0 - V3.0.2 wrote it as off, because YouTube refused its translations.
#define SUBTITLE_SETTING_BLOCK \
   "subtitle-language=auto\n" \
   "# the language subtitles are translated into when a video has none in it (Triangle offers that first):\n" \
   "# auto = the region's (Settings > Region; Czechia: Czech), a code such as cs, pl, sk, hu, de, en, or off.\n" \
   "# The player translates the video's own subtitles with Google Translate.\n"

static const char *OLD_SUBTITLE_SETTING_BLOCK =
   "subtitle-language=off\n"
   "# the language YouTube translates subtitles into when a video has none in it (Triangle offers it first):\n"
   "# off, auto = the region's (Czechia: Czech), or a code such as cs, pl, sk, hu, de, en. YouTube refuses\n"
   "# translations to this player at the moment (HTTP 429); the player says so when it does.\n";

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
   "# 720, 480, 360 or 144 - lower saves bandwidth on a slow connection. The player sets it too: START > Quality.\n"
   "\n"
   "update-check=on\n"
   "# on = at every start, read the newest release's version from the app's GitHub page and say when a newer\n"
   "# one is out (nothing is downloaded); off = never ask GitHub.\n"
   "\n"
   SUBTITLE_SETTING_BLOCK
   "\n"
   "subtitle-color=white\n"
   "subtitle-background=black\n"
   "subtitle-opacity=80\n"
   "subtitle-edge=none\n"
   "subtitle-size=100\n"
   "# how subtitles look - the player sets it too: START > Subtitle style. color and background: white, yellow,\n"
   "# green, cyan, blue, magenta, red or black; the background's opacity in per cent: 0, 20, 40, 60, 80 or 100;\n"
   "# edge: none, shadow or outline; size in per cent: 75, 100, 125, 150 or 200.\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;
static VideoDecoderSetting videoDecoder = VIDEO_DECODER_AUTO;
static int teeDecoderSpes = 4;
static int vp9Spes = 6;
static int downloadQuality = 0;   // download-quality=: the R3 page's last choice (downloads.h DownloadQuality)
static int qualityCap = 0;   // quality=: the highest video height to play, 0 = auto
static char subtitleLanguage[12] = "auto";   // (V3.0.0) subtitle-language=: off, auto or a code (V3.0.3: auto)
static int updateCheck = 1;                  // (V3.0.0 Test 12) update-check=: on / off

// ---- content region ----

// The countries the lists can come from, in the picker's order: the German-speaking three first, then A-Z by
// English name. Only these codes are ever sent - YouTube answers an unknown or lower-case gl with HTTP 200 and
// an empty page, not an error - and each one was probed in November 2026: its Trending lists and Live chips came
// back filled, apart from Live News in some (youtube.c falls back to a search there). Left out: India, where
// many titles are in scripts neither font of the console can draw, and China, which YouTube does not take. South
// Korea stays only while its Hangul titles prove legible on the console; deleting its line is the whole change -
// nothing else refers to a position in this table.
static const ContentRegion REGIONS[] = {
   { "DE", "Germany", "de" },
   { "AT", "Austria", "de" },
   { "CH", "Switzerland", "de" },
   { "AU", "Australia", "en" },
   { "BE", "Belgium", "nl" },
   { "BR", "Brazil", "pt" },
   { "BG", "Bulgaria", "bg" },
   { "CA", "Canada", "en" },
   { "HR", "Croatia", "hr" },
   { "CZ", "Czechia", "cs" },
   { "DK", "Denmark", "da" },
   { "FI", "Finland", "fi" },
   { "FR", "France", "fr" },
   { "GR", "Greece", "el" },
   { "HU", "Hungary", "hu" },
   { "IE", "Ireland", "en" },
   { "IT", "Italy", "it" },
   { "JP", "Japan", "ja" },
   { "MX", "Mexico", "es" },
   { "NL", "Netherlands", "nl" },
   { "NO", "Norway", "no" },
   { "PL", "Poland", "pl" },
   { "PT", "Portugal", "pt" },
   { "RO", "Romania", "ro" },
   { "RU", "Russia", "ru" },
   { "RS", "Serbia", "sr" },
   { "KR", "South Korea", "ko" },
   { "ES", "Spain", "es" },
   { "SE", "Sweden", "sv" },
   { "TR", "Turkey", "tr" },
   { "UA", "Ukraine", "uk" },
   { "GB", "United Kingdom", "en" },
   { "US", "United States", "en" },   // the default: what every earlier build sent
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

// (V3.0.3) A file V3.0.0 - V3.0.2 wrote holds subtitle-language=off with the comment that YouTube refuses its
// translations. The player translates subtitles itself now, and auto is the default: that block, exactly as it was
// written, becomes the new one - in the text read and in the file. A value someone set is left as it is, and so is
// a file read only in part (rewriting it would lose its tail).
static void migrateSubtitleSetting(const char *path, char *text, int cap, int length)
{
   char *found = strstr(text, OLD_SUBTITLE_SETTING_BLOCK);
   if (!found || length >= cap - 1) return;
   int oldLength = (int)strlen(OLD_SUBTITLE_SETTING_BLOCK), newLength = (int)strlen(SUBTITLE_SETTING_BLOCK);
   if (length - oldLength + newLength >= cap - 1) return;
   memmove(found + newLength, found + oldLength, (size_t)(length - (int)(found - text) - oldLength + 1));
   memcpy(found, SUBTITLE_SETTING_BLOCK, (size_t)newLength);
   if (writeFile(path, text, (uint64_t)strlen(text)) == 0)
      logInfo("[settings] subtitle-language off -> auto: subtitles are translated by the player now\n");
   else
      logWarn("[settings] subtitle-language off -> auto for this run; %s could not be rewritten\n", path);
}

// ---- the subtitle style (V3.0.3, asked for on PSX-Place) ----

typedef struct { const char *name, *value; uint32_t number; } StyleChoice;   // shown, saved, what it is
static const StyleChoice STYLE_COLORS[] = {          // the eight YouTube offers, for the text and its background
   { "White", "white", 0xFFFFFF }, { "Yellow", "yellow", 0xFFFF00 }, { "Green", "green", 0x00FF00 },
   { "Cyan", "cyan", 0x00FFFF }, { "Blue", "blue", 0x0000FF }, { "Magenta", "magenta", 0xFF00FF },
   { "Red", "red", 0xFF0000 }, { "Black", "black", 0x000000 },
};
static const StyleChoice STYLE_BACKGROUNDS[] = {
   { "Black", "black", 0x000000 }, { "White", "white", 0xFFFFFF }, { "Yellow", "yellow", 0xFFFF00 },
   { "Green", "green", 0x00FF00 }, { "Cyan", "cyan", 0x00FFFF }, { "Blue", "blue", 0x0000FF },
   { "Magenta", "magenta", 0xFF00FF }, { "Red", "red", 0xFF0000 },
};
static const StyleChoice STYLE_OPACITIES[] = {       // the alpha: 80% is near the plate subtitles always had
   { "0%", "0", 0 }, { "20%", "20", 51 }, { "40%", "40", 102 }, { "60%", "60", 153 }, { "80%", "80", 204 },
   { "100%", "100", 255 },
};
static const StyleChoice STYLE_EDGES[] = {
   { "None", "none", SUBTITLE_EDGE_NONE }, { "Shadow", "shadow", SUBTITLE_EDGE_SHADOW },
   { "Outline", "outline", SUBTITLE_EDGE_OUTLINE },
};
static const StyleChoice STYLE_SIZES[] = {           // per cent of the 30 px subtitles always had
   { "75%", "75", 22 }, { "100%", "100", 30 }, { "125%", "125", 38 }, { "150%", "150", 45 }, { "200%", "200", 60 },
};
#define STYLE_CHOICES(table) table, (int)(sizeof table / sizeof *table)
static const struct { const char *key; const StyleChoice *choices; int count; int fallback; }
STYLE_FIELDS[SUBTITLE_STYLE_FIELDS] = {
   { "subtitle-color",      STYLE_CHOICES(STYLE_COLORS),      0 },   // white
   { "subtitle-background", STYLE_CHOICES(STYLE_BACKGROUNDS), 0 },   // black
   { "subtitle-opacity",    STYLE_CHOICES(STYLE_OPACITIES),   4 },   // 80%
   { "subtitle-edge",       STYLE_CHOICES(STYLE_EDGES),       0 },   // none
   { "subtitle-size",       STYLE_CHOICES(STYLE_SIZES),       1 },   // 100%
};
static int styleChoice[SUBTITLE_STYLE_FIELDS] = { 0, 0, 4, 0, 1 };
static int styleSaved[SUBTITLE_STYLE_FIELDS]  = { 0, 0, 4, 0, 1 };   // what settings.txt holds

static void loadSubtitleStyle(const char *text)
{
   for (int field = 0; field < SUBTITLE_STYLE_FIELDS; field++) {
      const char *value = findSettingValue(text, STYLE_FIELDS[field].key);
      if (!value) continue;
      int found = -1;
      for (int i = 0; i < STYLE_FIELDS[field].count && found < 0; i++)
         if (settingValueEquals(value, STYLE_FIELDS[field].choices[i].value)) found = i;
      if (found < 0)
         logWarn("[settings] %s: unknown value, using %s\n", STYLE_FIELDS[field].key,
                 STYLE_FIELDS[field].choices[STYLE_FIELDS[field].fallback].value);
      styleChoice[field] = styleSaved[field] = found >= 0 ? found : STYLE_FIELDS[field].fallback;
   }
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
   if (length > 0) migrateSubtitleSetting(path, text, (int)sizeof text, length);

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
   // (V3.0.0) subtitle-language=: a code is letters and dashes ("cs", "pt-BR"), anything else is left at auto
   const char *subtitles = findSettingValue(text, "subtitle-language");
   if (subtitles) {
      char value[sizeof subtitleLanguage];
      int length = 0;
      while (length < (int)sizeof value - 1 && ((subtitles[length] >= 'a' && subtitles[length] <= 'z') ||
             (subtitles[length] >= 'A' && subtitles[length] <= 'Z') || subtitles[length] == '-')) {
         value[length] = subtitles[length];
         length++;
      }
      value[length] = 0;
      if (length >= 2) strCopy(subtitleLanguage, sizeof subtitleLanguage, value);
      else logWarn("[settings] subtitle-language takes off, auto or a code such as cs, using auto\n");
   }
   loadSubtitleStyle(text);   // (V3.0.3)
   const char *update = findSettingValue(text, "update-check");
   if (update) {
      if      (settingValueEquals(update, "on"))  updateCheck = 1;
      else if (settingValueEquals(update, "off")) updateCheck = 0;
      else logWarn("[settings] update-check takes on or off, using on\n");
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
int getUpdateCheck(void) { return updateCheck; }

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

const char *getSubtitleLanguage(void)
{
   if (strcmp(subtitleLanguage, "off") == 0) return "";
   if (strcmp(subtitleLanguage, "auto") != 0) return subtitleLanguage;
   const ContentRegion *region = getContentRegionAt(getContentRegionIndex());
   return region && region->language ? region->language : "en";
}

int setDownloadQuality(int quality)
{
   static const char *values[3] = { "1440", "1080", "xmb" };
   if (quality < 0 || quality > 2) return -1;
   if (quality == downloadQuality) return 0;
   downloadQuality = quality;
   return saveSetting("download-quality", values[quality]);
}

// ---- the subtitle style (V3.0.3) ----

static int styleField(SubtitleStyleField field) { return field >= 0 && field < SUBTITLE_STYLE_FIELDS; }

int getSubtitleStyleChoice(SubtitleStyleField field) { return styleField(field) ? styleChoice[field] : 0; }
int getSubtitleStyleChoiceCount(SubtitleStyleField field) { return styleField(field) ? STYLE_FIELDS[field].count : 1; }

const char *getSubtitleStyleChoiceName(SubtitleStyleField field, int choice)
{
   if (!styleField(field) || choice < 0 || choice >= STYLE_FIELDS[field].count) return "";
   return STYLE_FIELDS[field].choices[choice].name;
}

void setSubtitleStyleChoice(SubtitleStyleField field, int choice)
{
   if (styleField(field) && choice >= 0 && choice < STYLE_FIELDS[field].count) styleChoice[field] = choice;
}

int saveSubtitleStyle(void)
{
   int rc = 0, changed = 0;
   for (int field = 0; field < SUBTITLE_STYLE_FIELDS; field++) {
      if (styleChoice[field] == styleSaved[field]) continue;
      changed = 1;
      if (saveSetting(STYLE_FIELDS[field].key, STYLE_FIELDS[field].choices[styleChoice[field]].value) == 0)
         styleSaved[field] = styleChoice[field];
      else
         rc = -1;
   }
   if (changed)
      logInfo("[settings] subtitle style: %s on %s %s, edge %s, size %s (%s)\n", STYLE_COLORS[styleChoice[0]].value,
              STYLE_BACKGROUNDS[styleChoice[1]].value, STYLE_OPACITIES[styleChoice[2]].name, STYLE_EDGES[styleChoice[3]].value,
              STYLE_SIZES[styleChoice[4]].name, rc == 0 ? "saved" : "NOT saved");
   return rc;
}

uint32_t getSubtitleTextColor(void) { return 0xFF000000u | STYLE_COLORS[styleChoice[SUBTITLE_STYLE_COLOR]].number; }

uint32_t getSubtitleBackgroundColor(void)
{
   return (STYLE_OPACITIES[styleChoice[SUBTITLE_STYLE_OPACITY]].number << 24) |
          STYLE_BACKGROUNDS[styleChoice[SUBTITLE_STYLE_BACKGROUND]].number;
}

SubtitleEdge getSubtitleEdge(void) { return (SubtitleEdge)STYLE_EDGES[styleChoice[SUBTITLE_STYLE_EDGE]].number; }
int getSubtitleTextSize(void) { return (int)STYLE_SIZES[styleChoice[SUBTITLE_STYLE_SIZE]].number; }

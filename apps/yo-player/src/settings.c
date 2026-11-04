// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "settings.h"
#include "app-paths.h"
#include "settings-file.h"
#include "dbg.h"
#include <stdio.h>              // snprintf: the runtime path

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
   "# spaces become hyphens). the shipped one is tee; youtube-classic is the old palette.\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;

void loadSettings(void)
{
   // the directory migrateAppData settled on (see app-paths.h; main runs it first). Built from YO_DATA_DIR
   // the path was /dev_hdd0/yo-player even on a console that refuses that folder: the file was never created
   // there, every launch ran on the defaults, and theme.c read its theme= line from the data directory's
   // settings.txt - a different file from the one this function looked for.
   char path[128];
   snprintf(path, sizeof path, "%s/settings.txt", getAppDataDir());

   char text[2048];
   if (loadSettingsFile(path, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[settings] created %s with defaults\n", path);

   const char *mode = findSettingValue(text, "sponsorblock-mode");
   if (mode) {
      if      (settingValueEquals(mode, "off")) sponsorblockMode = SPONSORBLOCK_OFF;
      else if (settingValueEquals(mode, "ads")) sponsorblockMode = SPONSORBLOCK_ADS;
      else if (settingValueEquals(mode, "all")) sponsorblockMode = SPONSORBLOCK_ALL;
      else logWarn("[settings] unknown sponsorblock-mode value, using ads\n");
   }
}

SponsorblockMode getSponsorblockMode(void) { return sponsorblockMode; }

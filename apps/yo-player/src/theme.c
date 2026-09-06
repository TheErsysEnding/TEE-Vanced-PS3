// theme.c - yo-player's palette and its themes.txt file (see theme.h). The file format and the parsing
// live in simple-lib-app's theme-registry; this file is the built-in palette, the field table that maps
// its colours onto "key=" lines, and the two file paths.

#include "theme.h"
#include "app-paths.h"
#include "theme-registry.h"
#include "vfs.h"                // readFile / makeDir / fileExists
#include "settings-file.h"      // findSettingValue
#include "dbg.h"                // logInfo / logError
#include <string.h>             // strstr: spotting a pre-rebrand themes.txt
#include <stdio.h>              // snprintf: the runtime paths

// Paths are built at runtime from the directory migrateAppData settled on (see app-paths.h). Compiling
// them from YO_DATA_DIR looked identical and was wrong: on this console that folder cannot be created, so
// the theme file was written to - and read from - a directory that does not exist.
static char themesPath[96], themesBackup[96], settingsPath[96];

#define SETTINGS_BUFFER 4096

// TEE: the app's own palette, taken straight from the icon so the console entry and the app are one
// thing - #ff8a00 orange, the warm near-black ground and the cream text the user's other tools use
// (TEE Video2Audio, TEEconverter). The focus ring is orange with a faint wider ring outside it, which is
// as close to a glow as a renderer without blur gets.
static const Theme themeTee = {
   .appBg            = 0xFF0F0C07,
   .surface          = 0xFF221A10,
   .accent           = 0xFFFF8A00,
   .focusBorder      = 0xFFFF8A00,
   .focusGlow        = 0x3DFF8A00,
   .textPrimary      = 0xFFF3EADB,
   // Secondary text carries the button hints, the "Latest:" lines and every settings value, and it was
   // measured unreadable on the user's TV at 18px. A television is dimmer and further away than the
   // monitor this was picked on, so secondary sits much closer to primary here than a desktop palette
   // would put it - legibility first, hierarchy second.
   .textSecondary    = 0xFFCFC0A6,
   .badgeFill        = 0xCC0A0805,
   .scrim            = 0xD90A0805,
   .rowHighlight     = 0x33FF8A00,
   .seekTrack        = 0x59EADFCE,
   .seekNotch        = 0xFF0F0C07,
   .watchedThumbTint = 0xFF4A3B28,   // opaque so the focus ring can't bleed through a watched tile
   .focusThickness   = 4,
};

// every colour field by name, so a themes.txt "key=value" line maps straight onto the struct.
// focusThickness is not user-tunable - a block inherits the built-in's value via the seed copy.
static const ThemeColorField COLOR_FIELDS[] = {
   { "appBg",            offsetof(Theme, appBg)            },
   { "surface",          offsetof(Theme, surface)          },
   { "accent",           offsetof(Theme, accent)           },
   { "focusBorder",      offsetof(Theme, focusBorder)      },
   { "focusGlow",        offsetof(Theme, focusGlow)        },
   { "textPrimary",      offsetof(Theme, textPrimary)      },
   { "textSecondary",    offsetof(Theme, textSecondary)    },
   { "badgeFill",        offsetof(Theme, badgeFill)        },
   { "scrim",            offsetof(Theme, scrim)            },
   { "rowHighlight",     offsetof(Theme, rowHighlight)     },
   { "seekTrack",        offsetof(Theme, seekTrack)        },
   { "seekNotch",        offsetof(Theme, seekNotch)        },
   { "watchedThumbTint", offsetof(Theme, watchedThumbTint) },
};
#define COLOR_FIELD_COUNT ((int)(sizeof COLOR_FIELDS / sizeof COLOR_FIELDS[0]))

static const char *THEMES_FILE_HEADER =
   "# TEE Vanced themes - edit over FTP; changes apply on the next launch.\n"
   "#\n"
   "# Each [Name] block is one theme, and its name is what settings.txt selects with\n"
   "# \"theme=<name>\" (lower case, spaces become hyphens). Colours are #RRGGBB, or #RRGGBBAA\n"
   "# when you want transparency (AA comes last: 00 = clear, FF = solid). Every block inherits\n"
   "# TEE below, so a new one only has to list the colours it changes.\n"
   "\n"
   "# The palette this app shipped with before the rebrand. Select it with theme=youtube-classic.\n"
   "[YouTube Classic]\n"
   "appBg=#0F0F0F\n"
   "surface=#212121\n"
   "accent=#FF0000\n"
   "focusBorder=#FFFFFF\n"
   "focusGlow=#FFFFFF28\n"
   "textPrimary=#FFFFFF\n"
   "textSecondary=#D0D0D0\n"
   "rowHighlight=#FFFFFF28\n"
   "seekTrack=#FFFFFF66\n"
   "seekNotch=#000000\n"
   "watchedThumbTint=#4D4D4D\n\n";

static Theme         themes[MAX_THEMES];
static ThemeRegistry registry;

const Theme *activeTheme = &themeTee;

// selects the theme settings.txt names; an absent or unknown name leaves the built-in in place.
static void applySettingsTheme(void)
{
   char text[SETTINGS_BUFFER];
   int length = readFile(settingsPath, text, sizeof text - 1);
   if (length <= 0) return;
   text[length] = '\0';

   const char *slug = findSettingValue(text, "theme");
   int themeIndex = slug ? findThemeBySlug(&registry, slug) : -1;
   if (themeIndex >= 0) activeTheme = (const Theme *)getRegisteredTheme(&registry, themeIndex);
}

void initTheme(void)
{
   const char *dir = getAppDataDir();
   snprintf(themesPath,   sizeof themesPath,   "%s/themes.txt",     dir);
   snprintf(themesBackup, sizeof themesBackup, "%s/themes.txt.bak", dir);
   snprintf(settingsPath, sizeof settingsPath, "%s/settings.txt",   dir);
   makeDir(dir);

   initThemeRegistry(&registry, themes, sizeof(Theme), &themeTee, "TEE", COLOR_FIELDS, COLOR_FIELD_COUNT);
   activeTheme = (const Theme *)getRegisteredTheme(&registry, 0);   // the guaranteed fallback

   // First launch writes the built-in out in full, so every key is there to edit.
   //
   // A file from before the rebrand is a special case: it has no [TEE] block at all, and its selected
   // theme still names the old palette - so an existing install would keep the old skin for ever and the
   // new one would look like it had failed. Such a file is replaced, but only after it has been copied to
   // themes.txt.bak, so a hand-tuned palette is never simply thrown away.
   char probe[SETTINGS_BUFFER];
   int existing = fileExists(themesPath) ? readFile(themesPath, probe, sizeof probe - 1) : -1;
   int stale = 0;
   if (existing > 0) {
      probe[existing] = '\0';
      stale = strstr(probe, "[TEE]") == NULL;
      if (stale) {
         // copyFile wants the caller's scratch buffer; probe is free at this point (its contents have
         // already been reduced to the `stale` verdict above).
         if (copyFile(themesPath, themesBackup, probe, sizeof probe) == 0)
            logInfo("[theme] kept the old palette as %s\n", themesBackup);
         else
            logError("[theme] could not back up %s\n", themesPath);
      }
   }
   if (existing <= 0 || stale) {
      int rc = writeThemeTemplate(&registry, themesPath, THEMES_FILE_HEADER);
      if (rc == 0) logInfo("[theme] wrote %s (%s)\n", themesPath, stale ? "replaced a pre-rebrand file" : "first launch");
      else         logError("[theme] couldn't create %s\n", themesPath);
   }

   loadThemeFile(&registry, themesPath);
   applySettingsTheme();
}

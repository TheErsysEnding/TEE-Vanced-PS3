// theme.c - yo-player's palette and its themes.txt file (see theme.h). The file format and the parsing
// live in simple-lib-app's theme-registry; this file is the built-in palette, the field table that maps
// its colours onto "key=" lines, and the two file paths.

#include "theme.h"
#include "app-paths.h"
#include "theme-registry.h"
#include "vfs.h"                // readFile / makeDir / fileExists
#include "settings-file.h"      // findSettingValue
#include "dbg.h"                // logInfo / logError
#include <string.h>             // strstr: spotting a themes.txt from an older version
#include <stdio.h>              // snprintf: the runtime paths

// Paths are built at runtime from the directory migrateAppData settled on (see app-paths.h). Compiling
// them from YO_DATA_DIR looked identical and was wrong: on this console that folder cannot be created, so
// the theme file was written to - and read from - a directory that does not exist.
static char themesPath[96], themesBackup[96], settingsPath[96];

#define SETTINGS_BUFFER 4096

// TEE: the app's own palette, taken straight from the icon so the console entry and the app are one
// thing - #ff8a00 orange, the warm near-black ground and the cream text the user's other tools use
// (TEE Video2Audio, TEEconverter). Since v1.0 these are the "Ember" design tokens (UEBERGABE 3.1/3.2):
// the same orange, a slightly cooler ground, and helper textures instead of flat rectangles - the focus
// glow is now a soft texture tinted with focusGlow rather than a second, wider ring.
static const Theme themeTee = {
   .appBg            = 0xFF0E0C0A,
   .surface          = 0xFF211C16,
   .accent           = 0xFFFF8A00,
   .focusBorder      = 0xFFFF8A00,
   .focusGlow        = 0x73FF8A00,
   .textPrimary      = 0xFFF5EFE6,
   // Secondary text carries the button hints, the "Latest:" lines and every settings value, and it was
   // measured unreadable on the user's TV at 18px. A television is dimmer and further away than the
   // monitor this was picked on, so secondary sits much closer to primary here than a desktop palette
   // would put it - legibility first, hierarchy second. Ember's text-2 is a little darker than the old
   // value, which is why the same release raises the smallest reading text to 20 px.
   .textSecondary    = 0xFFBDB3A5,
   .badgeFill        = 0xD60A0806,
   .scrim            = 0xD90A0805,
   .rowHighlight     = 0x33FF8A00,
   .seekTrack        = 0x4DF5EFE6,
   .seekNotch        = 0xFF0E0C0A,
   .watchedThumbTint = 0xFF8C8C8C,   // 55 % brightness, opaque so the focus ring can't bleed through a watched tile
   .surface1         = 0xFF17140F,
   .surface3         = 0xFF2C251D,
   .textTertiary     = 0xFF8F8578,   // only at 20 px or more, for the same reason as textSecondary above
   .accentSoft       = 0x33FF8A00,
   .accentText       = 0xFFFFAF52,
   .onAccent         = 0xFF1A1208,
   .live             = 0xFFE5484D,
   .knobGlow         = 0x80FF8A00,
   .hintFill         = 0x1FF5EFE6,
   .hintFillOnVideo  = 0x24F5EFE6,
   .chipOnVideo      = 0xB814100C,
   .panelOnVideo     = 0xDB14100C,
   .progressTrack    = 0x47F5EFE6,
   .hairline         = 0x14F5EFE6,
   .gradientBase     = 0xFF080604,
   .letterbox        = 0xFF000000,   // video bars stay black whatever the theme: a tinted bar reads as part of the picture
   .focusThickness   = 4,
};

// every colour field by name, so a themes.txt "key=value" line maps straight onto the struct.
// focusThickness is not user-tunable - a block inherits the built-in's value via the seed copy. The keys
// added in v1.0 come last, so the lines an older file already had keep their order.
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
   { "surface1",         offsetof(Theme, surface1)         },
   { "surface3",         offsetof(Theme, surface3)         },
   { "textTertiary",     offsetof(Theme, textTertiary)     },
   { "accentSoft",       offsetof(Theme, accentSoft)       },
   { "accentText",       offsetof(Theme, accentText)       },
   { "onAccent",         offsetof(Theme, onAccent)         },
   { "live",             offsetof(Theme, live)             },
   { "knobGlow",         offsetof(Theme, knobGlow)         },
   { "hintFill",         offsetof(Theme, hintFill)         },
   { "hintFillOnVideo",  offsetof(Theme, hintFillOnVideo)  },
   { "chipOnVideo",      offsetof(Theme, chipOnVideo)      },
   { "panelOnVideo",     offsetof(Theme, panelOnVideo)     },
   { "progressTrack",    offsetof(Theme, progressTrack)    },
   { "hairline",         offsetof(Theme, hairline)         },
   { "gradientBase",     offsetof(Theme, gradientBase)     },
   { "letterbox",        offsetof(Theme, letterbox)        },
};
#define COLOR_FIELD_COUNT ((int)(sizeof COLOR_FIELDS / sizeof COLOR_FIELDS[0]))

// Says a themes.txt was written for the Ember palette. A file without it is from before v1.0 and still
// holds the old [TEE] colours, which would override the new built-in on every launch - the new look would
// simply never appear. initTheme treats such a file the way it treats one from before the rebrand: copied
// to themes.txt.bak, then replaced. The marker is on the header's second line because the check only reads
// the first 4 KB of the file.
#define THEMES_FILE_MARKER "palette: ember-1"

static const char *THEMES_FILE_HEADER =
   "# TEE Vanced themes - edit over FTP; changes apply on the next launch.\n"
   "# " THEMES_FILE_MARKER " - keep this line: without it the app takes the file for an old one and\n"
   "# replaces it (after copying it to themes.txt.bak).\n"
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
   "watchedThumbTint=#4D4D4D\n"
   // the v1.0 keys: without them this block would inherit TEE's orange highlights next to its red
   "surface1=#181818\n"
   "surface3=#383838\n"
   "textTertiary=#AAAAAA\n"
   "accentSoft=#FF000033\n"
   "accentText=#FF7A7A\n"
   "onAccent=#FFFFFF\n"
   "live=#FF0000\n"
   "knobGlow=#FF000080\n"
   "hintFill=#FFFFFF1F\n"
   "hintFillOnVideo=#FFFFFF24\n"
   "chipOnVideo=#000000B8\n"
   "panelOnVideo=#000000DB\n"
   "progressTrack=#FFFFFF47\n"
   "hairline=#FFFFFF14\n"
   "gradientBase=#000000\n"
   "letterbox=#000000\n\n";

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
   // new one would look like it had failed. A file from before v1.0 is the same problem one step later:
   // it HAS a [TEE] block, but with the old palette in it, and that block overrides the built-in - so the
   // Ember colours would never show. Both are told apart by the missing THEMES_FILE_MARKER alone - a file
   // from before the rebrand predates it as well - so a current file is kept even when the user has deleted
   // its [TEE] block or pushed it past the 4 KB read below; "[TEE]" only names the case in the log. Either
   // file is replaced, but only after it has been copied to themes.txt.bak, so a hand-tuned palette is never
   // simply thrown away.
   char probe[SETTINGS_BUFFER];
   int present  = fileExists(themesPath);
   int existing = present ? readFile(themesPath, probe, sizeof probe - 1) : -1;
   int stale = 0, preRebrand = 0, backedUp = 0;
   if (existing > 0) {
      probe[existing] = '\0';
      preRebrand = strstr(probe, "[TEE]") == NULL;
      stale = strstr(probe, THEMES_FILE_MARKER) == NULL;
      if (stale) {
         // copyFile wants the caller's scratch buffer; probe is free at this point (its contents have
         // already been reduced to the `stale` verdict above).
         backedUp = copyFile(themesPath, themesBackup, probe, sizeof probe) == 0;
         if (backedUp)
            logInfo("[theme] kept the old palette as %s\n", themesBackup);
         else
            logError("[theme] could not back up %s - left it as it is, built-in palette this run\n", themesPath);
      }
   } else if (present && existing < 0) {
      // a file that is there but cannot be read was never checked for the marker - it may be a current one -
      // so it is neither backed up (copyFile would have to read it too) nor overwritten
      logError("[theme] could not read %s - left it as it is\n", themesPath);
   }
   if (!present || existing == 0 || backedUp) {
      const char *reason = !stale ? "first launch" : preRebrand ? "replaced a pre-rebrand file" : "replaced a pre-Ember file";
      int rc = writeThemeTemplate(&registry, themesPath, THEMES_FILE_HEADER);
      if (rc == 0) logInfo("[theme] wrote %s (%s)\n", themesPath, reason);
      else         logError("[theme] couldn't create %s\n", themesPath);
   }

   // A stale file that could not be backed up is neither replaced nor applied: its old colours would hide the
   // Ember palette, so the built-in applies this run and the next launch tries the backup again. An
   // unreadable file is harmless here - loadThemeFile's own read just fails.
   if (!stale || backedUp) loadThemeFile(&registry, themesPath);
   applySettingsTheme();
}

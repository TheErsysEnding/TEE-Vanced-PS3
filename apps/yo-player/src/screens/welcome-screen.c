// welcome-screen - a new version's changes and the support page, once per version (see welcome-screen.h).
//
// Three pages on the app's ground, inside the safe area, in the language chosen at the first start (V3.1.0: Home asks
// for it before this). Page 1: 4K30, new and still a beta - it will very likely fall back to 1440p30 often - with the
// link and QR code to tell TEE how it went (TEE: "es soll ein zusätzliches Bild am Anfang kommen in dem steht das 4K30
// fps hinzugefügt wurde es aber sehr wahrscheinlich oft fallbacken wird auf 1440p30 bitte teilt eure Erfahrung mit:
// linktr.ee/theersysending"). Page 2: the version's other changes. Page 3: "Support TheErsysEnding:" with the link and
// the QR code the About card shows. X goes on (and closes the last page), O closes at once.

#include "screens/welcome-screen.h"
#include "app-paths.h"          // APP_VERSION, getAppDataDir
#include "vfs.h"                // readFile / writeFile
#include "ui-hints.h"           // UiHints; brings ui-draw.h and ui-fonts.h
#include "qr-linktree.h"        // drawQrCode, QR_URL
#include "i18n.h"               // (V3.1.0) tr
#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <string.h>
#include <stdio.h>

#define LEFT_X        176                  // where every Ember page's content starts
#define TEXT_W        (UI_SAFE_RIGHT - LEFT_X)
#define VERSION_Y     110                  // the version line's baseline
#define TITLE_SIZE    104                  // "4K 30 FPS"
#define TITLE_GAP     118                  // version baseline -> the title's baseline
#define HEADING_SIZE  72                   // page 2's "What's new"
#define LEAD_SIZE     30
#define NOTE_SIZE     26
#define LIST_SIZE     24
#define BLOCK_GAP     34                   // between the blocks of text
#define NOTE_PAD      28                   // inside the notes' panel
#define QR_SCALE      12                   // page 3: 37 modules -> 444 px
#define QR_SCALE_4K   10                   // page 1: 370 px, beside the text
#define QR_GAP        64                   // page 1: the text -> the code
#define LINK_SIZE     40
#define ITEM_GAP      12                   // between two points of a list
#define BULLET        "\xE2\x80\xA2  "     // "•  "

#define FILE_NAME     "welcome.txt"

// one label each; translated, a point may pass a Label's 255 bytes (uiInitLongLabel)
static const char *CHANGES[] = {
   N_("The app in 22 languages, Arabic right to left: choose yours at the first start, or later in Settings > Language."),
   N_("1440p60 drops to 1080p60 within about two seconds in a busy scene, and a start too heavy for 1440p begins in 1080p60 "
      "at once."),
   N_("4K30 videos start in 1440p and go up to 4K where the video is light enough."),
   N_("Arabic and Hebrew video titles are shown now, right to left."),
};
#define CHANGE_COUNT ((int)(sizeof CHANGES / sizeof CHANGES[0]))

static Label   versionLabel, titleLabel, leadLabel, noteLabel, askLabel, link4kLabel;   // page 1
static Label   headingLabel, changesLabels[CHANGE_COUNT];                              // page 2
static Label   supportLabel, linkLabel, scanLabel;                                     // page 3
static UiHints nextHints, closeHints;
static int     page;   // 0 4K30, 1 the changes, 2 the support page

// "V3.1.0 Test 13" -> "3.1.0", "V3.1.0 4K30 BETA" -> "3.1.0 4K30 BETA": a version is welcomed once, its test builds with it
static void baseVersion(char *out, int cap)
{
   const char *v = APP_VERSION;
   if (*v == 'V' || *v == 'v') v++;
   const char *test = strstr(v, " Test");
   int n = 0, end = test ? (int)(test - v) : (int)strlen(v);
   while (n < end && n < cap - 1) { out[n] = v[n]; n++; }
   out[n] = 0;
}

static void welcomePath(char *out, int cap) { snprintf(out, cap, "%s/%s", getAppDataDir(), FILE_NAME); }

int isWelcomeDue(void)
{
   char path[160], seen[48], version[48];
   welcomePath(path, sizeof path);
   baseVersion(version, sizeof version);
   int length = readFile(path, seen, sizeof seen - 1);
   if (length < 0) length = 0;
   seen[length] = 0;
   while (length > 0 && (seen[length - 1] == '\n' || seen[length - 1] == '\r')) seen[--length] = 0;
   return strcmp(seen, version) != 0;
}

static void markWelcomed(void)
{
   char path[160], version[48];
   welcomePath(path, sizeof path);
   baseVersion(version, sizeof version);
   if (writeFile(path, version, (uint64_t)strlen(version)) != 0) logWarn("[welcome] %s could not be written\n", path);
   else logInfo("[welcome] version %s welcomed\n", version);
}

static int qrSpan(int scale) { return (QR_SIZE + 2 * QR_QUIET) * scale; }

static void initWelcome(void)
{
   page = 0;
   char version[160];
   snprintf(version, sizeof version, tr("TEE Vanced %s  \xC2\xB7  what's new"), APP_VERSION);
   uiInitLabel(&versionLabel, UI_WEIGHT_BOLD, 0, 0, TEXT_W, 26, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, version);

   // page 1: 4K30
   int columnW = TEXT_W - qrSpan(QR_SCALE_4K) - QR_GAP;
   uiInitTextLabel(&titleLabel, UI_WEIGHT_EXTRABOLD, 0, 0, columnW, TITLE_SIZE, activeTheme->accent, TEXT_NOWRAP, "4K 30 FPS");
   uiInitLongLabel(&leadLabel, UI_WEIGHT_SEMIBOLD, 0, 0, columnW, LEAD_SIZE, activeTheme->textPrimary, TEXT_WRAP,
                   tr("New: 4K videos at 30 frames a second, on the SPEs with the TEE VP9 decoder."));
   uiInitLongLabel(&noteLabel, UI_WEIGHT_SEMIBOLD, 0, 0, columnW - 2 * NOTE_PAD, NOTE_SIZE, activeTheme->textPrimary,
                   TEXT_WRAP, tr("4K30 is a beta: it will very likely fall back to 1440p30 often. Busy scenes in 4K are "
                                 "more than the PS3 can decode in time."));
   uiInitLongLabel(&askLabel, UI_WEIGHT_SEMIBOLD, 0, 0, columnW, LEAD_SIZE, activeTheme->textSecondary, TEXT_WRAP,
                   tr("Please share your experience with it:"));
   uiInitTextLabel(&link4kLabel, UI_WEIGHT_EXTRABOLD, 0, 0, columnW, LINK_SIZE, activeTheme->accent, TEXT_NOWRAP, QR_URL);

   // page 2: the changes
   uiInitLabel(&headingLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W, HEADING_SIZE, activeTheme->accent, TEXT_NOWRAP_ELLIPSIS,
               tr("What's new"));
   for (int i = 0; i < CHANGE_COUNT; i++) {
      char line[1024];
      snprintf(line, sizeof line, "%s%s", BULLET, tr(CHANGES[i]));
      uiInitLongLabel(&changesLabels[i], UI_WEIGHT_SEMIBOLD, 0, 0, TEXT_W, NOTE_SIZE, activeTheme->textPrimary, TEXT_WRAP, line);
   }

   // page 3: support
   uiInitLabel(&supportLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W - qrSpan(QR_SCALE) - QR_GAP, 72, activeTheme->textPrimary,
               TEXT_WRAP, tr("Support TheErsysEnding:"));
   uiInitTextLabel(&linkLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W, LINK_SIZE, activeTheme->accent, TEXT_NOWRAP, QR_URL);
   uiInitLongLabel(&scanLabel, UI_WEIGHT_SEMIBOLD, 0, 0, 760, LEAD_SIZE, activeTheme->textSecondary, TEXT_WRAP,
                   tr("Scan the code with your phone: links, streams and contact."));
   uiInitHints(&nextHints);
   uiAddHint(&nextHints, GLYPH_CROSS, tr("Continue"));
   uiInitHints(&closeHints);
   uiAddHint(&closeHints, GLYPH_CROSS, tr("Close"));
}

static void updateWelcome(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || (page == 2 && isPadButtonPressed(PAD_BTN_CROSS))) {
      markWelcomed();
      popScreen();
      return;
   }
   if (page < 2 && isPadButtonPressed(PAD_BTN_CROSS)) page++;
}

// a label placed by its first baseline; returns the baseline under its last line
static int drawAt(Label *label, int x, int baseline)
{
   uiDrawLabelAt(label, x, uiLabelYForBaseline(label, baseline));
   UiTextMetrics m = uiGetTextMetrics(label->font, label->size);
   int extra = label->tt.tex.h - m.texH;
   return baseline + (extra > 0 ? extra : 0);
}

static void drawWelcome(void)
{
   int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
   fillGfxRectangle(0, 0, screenW, screenH, activeTheme->appBg);
   if (page == 0) {
      int base = drawAt(&versionLabel, LEFT_X, VERSION_Y);
      base = drawAt(&titleLabel, LEFT_X, base + TITLE_GAP);
      int qrTop = base + 40;
      base = drawAt(&leadLabel, LEFT_X, base + 66);
      // the beta note on a panel of its own: it is what to expect of 4K30
      int panelY = base + BLOCK_GAP;
      int columnW = TEXT_W - qrSpan(QR_SCALE_4K) - QR_GAP;
      int panelH = 2 * NOTE_PAD + noteLabel.tt.tex.h;
      uiFillRounded(LEFT_X - 4, panelY, columnW + 8, panelH, UI_RADIUS_PANEL, activeTheme->surface);
      uiDrawLabelAt(&noteLabel, LEFT_X + NOTE_PAD, panelY + NOTE_PAD);
      base = drawAt(&askLabel, LEFT_X, panelY + panelH + BLOCK_GAP + 30);
      drawAt(&link4kLabel, LEFT_X, base + 60);
      drawQrCode(UI_SAFE_RIGHT - qrSpan(QR_SCALE_4K), qrTop, QR_SCALE_4K);
      uiDrawHints(&nextHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   } else if (page == 1) {
      int base = drawAt(&versionLabel, LEFT_X, VERSION_Y);
      base = drawAt(&headingLabel, LEFT_X, base + 100);
      int top = base + BLOCK_GAP + 10;
      for (int i = 0; i < CHANGE_COUNT; i++) {
         uiDrawLabelAt(&changesLabels[i], LEFT_X, top);
         top += changesLabels[i].tt.tex.h + 2 * ITEM_GAP;
      }
      uiDrawHints(&nextHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   } else {
      int span = qrSpan(QR_SCALE);
      int qrX = UI_SAFE_RIGHT - span, qrY = (screenH - span) / 2;
      int base = drawAt(&supportLabel, LEFT_X, qrY + 90);
      base = drawAt(&linkLabel, LEFT_X, base + 90);
      drawAt(&scanLabel, LEFT_X, base + 70);
      drawQrCode(qrX, qrY, QR_SCALE);
      uiDrawHints(&closeHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   }
}

static void termWelcome(void)
{
   freeLabel(&versionLabel); freeLabel(&titleLabel); freeLabel(&leadLabel); freeLabel(&noteLabel); freeLabel(&askLabel);
   freeLabel(&link4kLabel); freeLabel(&headingLabel);
   for (int i = 0; i < CHANGE_COUNT; i++) freeLabel(&changesLabels[i]);
   freeLabel(&supportLabel); freeLabel(&linkLabel); freeLabel(&scanLabel);
   uiTermHints(&nextHints);
   uiTermHints(&closeHints);
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen welcomeScreen = { initWelcome, NULL, updateWelcome, drawWelcome, NULL, termWelcome, SCREEN_TERMINATED };

void openWelcome(void) { pushScreen(&welcomeScreen); }

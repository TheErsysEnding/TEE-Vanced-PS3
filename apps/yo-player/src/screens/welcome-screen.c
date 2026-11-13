// welcome-screen - a new version's changes and the support page, once per version (see welcome-screen.h).
//
// Two pages on the app's ground, inside the safe area. Page 1: the version, "1440p 60 FPS SUPPORT" as large as the
// screen allows - nobody had played 1440p60 natively on a PS3 before - two notes on what to expect of it, and the
// version's other changes. Page 2: "Support TheErsysEnding:" with the link and the QR code the About card shows.
// X goes on (and closes the last page), O closes at once. All text is the app's own, in English.

#include "screens/welcome-screen.h"
#include "app-paths.h"          // APP_VERSION, getAppDataDir
#include "vfs.h"                // readFile / writeFile
#include "ui-hints.h"           // UiHints; brings ui-draw.h and ui-fonts.h
#include "qr-linktree.h"        // drawQrCode, QR_URL
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
#define TITLE_SIZE    104                  // "1440p 60 FPS SUPPORT"
#define TITLE_GAP     118                  // version baseline -> the title's baseline
#define LEAD_SIZE     30
#define NOTE_SIZE     26
#define LIST_SIZE     24
#define BLOCK_GAP     34                   // between the blocks of text
#define NOTE_PAD      28                   // inside the notes' panel
#define QR_SCALE      12                   // page 2: 37 modules -> 444 px
#define BULLET        "\xE2\x80\xA2  "     // "•  "

#define FILE_NAME     "welcome.txt"

// one label each: a label holds 255 bytes of text
static const char *NOTES[] = {
   BULLET "In the busiest moments a 1440p60 video can drop back to 1080p60 for a few seconds - on its own, at the "
   "next key frame - and come back after them: the picture keeps 60 frames a second, the sound never stutters.",
   BULLET "1440p30 videos are very likely to play stable all the way, without dropping back.",
};
static const char *CHANGES[] = {
   BULLET "Audio tracks: AI-dubbed videos play their original sound, in 1440p too - Square lists every track.",
   BULLET "List view: DOWN on Home, every search and every channel page list the whole feed - each video's best "
   "quality (1440p60, 1080p60 ...), its details and description on the right, Subscribe right there.",
   BULLET "Downloads: 1440p or 1080p60 into the app's own Downloads page, or an MP4 into the XMB - progress in the "
   "rail, a note when one is in or failed, unfinished ones found at the next start.",
   BULLET "Subscriptions with every channel's picture; subtitles in Czech, Polish, Turkish, Greek, Cyrillic and more.",
   BULLET "A new blue look, the BD Remote Control - PREV/NEXT step chapter by chapter - and a note when a new version is out.",
};
#define NOTE_COUNT   ((int)(sizeof NOTES / sizeof NOTES[0]))
#define CHANGE_COUNT ((int)(sizeof CHANGES / sizeof CHANGES[0]))
#define ITEM_GAP     12   // between two points of a list

static Label   versionLabel, titleLabel, leadLabel, notesLabels[NOTE_COUNT], changesLabels[CHANGE_COUNT];
static Label   supportLabel, linkLabel, scanLabel;
static UiHints nextHints, closeHints;
static int     page;   // 0 the changes, 1 the support page

// "V3.0.0 Test 15 ..." -> "3.0.0": a version is welcomed once, its test builds with it
static void baseVersion(char *out, int cap)
{
   const char *v = APP_VERSION;
   if (*v == 'V' || *v == 'v') v++;
   int n = 0;
   while (v[n] && v[n] != ' ' && n < cap - 1) { out[n] = v[n]; n++; }
   out[n] = 0;
}

static void welcomePath(char *out, int cap) { snprintf(out, cap, "%s/%s", getAppDataDir(), FILE_NAME); }

int isWelcomeDue(void)
{
   char path[160], seen[32], version[32];
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
   char path[160], version[32];
   welcomePath(path, sizeof path);
   baseVersion(version, sizeof version);
   if (writeFile(path, version, (uint64_t)strlen(version)) != 0) logWarn("[welcome] %s could not be written\n", path);
   else logInfo("[welcome] version %s welcomed\n", version);
}

static void initWelcome(void)
{
   page = 0;
   char version[64];
   snprintf(version, sizeof version, "TEE Vanced " APP_VERSION "  \xC2\xB7  what's new");
   uiInitLabel(&versionLabel, UI_WEIGHT_BOLD, 0, 0, TEXT_W, 26, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, version);
   uiInitLabel(&titleLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W, TITLE_SIZE, activeTheme->accent, TEXT_NOWRAP,
               "1440p 60 FPS SUPPORT");
   uiInitLabel(&leadLabel, UI_WEIGHT_SEMIBOLD, 0, 0, TEXT_W, LEAD_SIZE, activeTheme->textPrimary, TEXT_WRAP,
               "Native 1440p at 60 frames a second on the PS3: VP9, decoded on the SPEs by the TEE VP9 decoder, with "
               "Opus sound.");
   for (int i = 0; i < NOTE_COUNT; i++)
      uiInitLabel(&notesLabels[i], UI_WEIGHT_SEMIBOLD, 0, 0, TEXT_W - 2 * NOTE_PAD, NOTE_SIZE, activeTheme->textPrimary,
                  TEXT_WRAP, NOTES[i]);
   for (int i = 0; i < CHANGE_COUNT; i++)
      uiInitLabel(&changesLabels[i], UI_BODY_WEIGHT, 0, 0, TEXT_W, LIST_SIZE, activeTheme->textSecondary, TEXT_WRAP,
                  CHANGES[i]);

   uiInitLabel(&supportLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W, 72, activeTheme->textPrimary, TEXT_NOWRAP,
               "Support TheErsysEnding:");
   uiInitLabel(&linkLabel, UI_WEIGHT_EXTRABOLD, 0, 0, TEXT_W, 40, activeTheme->accent, TEXT_NOWRAP, QR_URL);
   uiInitLabel(&scanLabel, UI_WEIGHT_SEMIBOLD, 0, 0, 760, LEAD_SIZE, activeTheme->textSecondary, TEXT_WRAP,
               "Scan the code with your phone: links, streams and contact.");
   uiInitHints(&nextHints);
   uiAddHint(&nextHints, GLYPH_CROSS, "Continue");
   uiInitHints(&closeHints);
   uiAddHint(&closeHints, GLYPH_CROSS, "Close");
}

static void updateWelcome(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || (page == 1 && isPadButtonPressed(PAD_BTN_CROSS))) {
      markWelcomed();
      popScreen();
      return;
   }
   if (page == 0 && isPadButtonPressed(PAD_BTN_CROSS)) page = 1;
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
      base = drawAt(&leadLabel, LEFT_X, base + 66);
      // the two notes on a panel of their own: they are what to expect of 1440p60
      int panelY = base + BLOCK_GAP;
      int notesH = 2 * NOTE_PAD - ITEM_GAP;
      for (int i = 0; i < NOTE_COUNT; i++) notesH += notesLabels[i].tt.tex.h + ITEM_GAP;
      uiFillRounded(LEFT_X - 4, panelY, TEXT_W + 8, notesH, UI_RADIUS_PANEL, activeTheme->surface);
      int top = panelY + NOTE_PAD;
      for (int i = 0; i < NOTE_COUNT; i++) {
         uiDrawLabelAt(&notesLabels[i], LEFT_X + NOTE_PAD, top);
         top += notesLabels[i].tt.tex.h + ITEM_GAP;
      }
      top = panelY + notesH + BLOCK_GAP;
      for (int i = 0; i < CHANGE_COUNT; i++) {
         uiDrawLabelAt(&changesLabels[i], LEFT_X, top);
         top += changesLabels[i].tt.tex.h + ITEM_GAP;
      }
      uiDrawHints(&nextHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   } else {
      int qrSpan = (QR_SIZE + 2 * QR_QUIET) * QR_SCALE;
      int qrX = UI_SAFE_RIGHT - qrSpan, qrY = (screenH - qrSpan) / 2;
      int base = drawAt(&supportLabel, LEFT_X, qrY + 90);
      base = drawAt(&linkLabel, LEFT_X, base + 90);
      drawAt(&scanLabel, LEFT_X, base + 70);
      drawQrCode(qrX, qrY, QR_SCALE);
      uiDrawHints(&closeHints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);
   }
}

static void termWelcome(void)
{
   freeLabel(&versionLabel); freeLabel(&titleLabel); freeLabel(&leadLabel);
   for (int i = 0; i < NOTE_COUNT; i++) freeLabel(&notesLabels[i]);
   for (int i = 0; i < CHANGE_COUNT; i++) freeLabel(&changesLabels[i]);
   freeLabel(&supportLabel); freeLabel(&linkLabel); freeLabel(&scanLabel);
   uiTermHints(&nextHints);
   uiTermHints(&closeHints);
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen welcomeScreen = { initWelcome, NULL, updateWelcome, drawWelcome, NULL, termWelcome, SCREEN_TERMINATED };

void openWelcome(void) { pushScreen(&welcomeScreen); }

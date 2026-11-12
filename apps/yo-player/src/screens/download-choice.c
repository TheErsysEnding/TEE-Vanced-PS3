// download-choice - R3 on a video: in which form to download it (see download-choice.h).
//
// Three rows like the playlist chooser's, the last choice preselected (settings.txt download-quality=): the two
// that only TEE Vanced plays - 1440p60 VP9 on TEE VP9, 1080p60 H.264 on the TEE Decoder - land in the app's own
// Downloads page; the third is the remuxed MP4 the XMB's own player keeps up with, handed to its Video column as
// before. X downloads, O goes back.

#include "screens/download-choice.h"
#include "downloads.h"
#include "settings.h"           // getDownloadQuality / setDownloadQuality, getVp9Spes
#include "ui-hints.h"           // UiHints; brings ui-draw.h and ui-fonts.h
#include "gfx.h"
#include "theme.h"
#include "anim.h"               // interpolateColor: the focused plate's colour
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "button-repeat.h"
#include "screen-manager.h"
#include <string.h>

#define CONTENT_X      176
#define HEADER_Y       67
#define HEADER_H       45
#define SUBTITLE_BASE  158      // the video's title, under the page title
#define LIST_TOP       210
#define ROW_H          96
#define PLATE_H        (ROW_H - 12)
#define LIST_W         1152
#define ROW_PAD        24
#define ICON_SIZE      26
#define ICON_GAP       16
#define NAME_SIZE      26       // "1440p 60 fps · VP9", 26/800
#define NOTE_SIZE      20       // what it is for, 20/600 in text-2

static const char *CHOICE_NAMES[DOWNLOAD_QUALITY_COUNT] = {
   "1440p 60 fps  ·  VP9",
   "1080p 60 fps  ·  H.264",
   "720p60 / 1080p30  ·  MP4 for the XMB",
};

static SearchResult pendingItem;
static Label        titleLabel, videoLabel, nameLabels[DOWNLOAD_QUALITY_COUNT], noteLabels[DOWNLOAD_QUALITY_COUNT];
static Icon         rowIcon;
static UiHints      hints;
static ButtonRepeat navigateRepeat;
static struct {
   int screenW, screenH;
   int selected;
   int warning;            // the XMB row was picked: its warning is up (X downloads for the XMB, O goes back)
   uint32_t plateFill;
} state;
static Label   warningLabel;
static UiHints warningHints;

static void initDownloadChoice(void)
{
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   // TEE: into the app's own gallery unless asked otherwise - the XMB is a choice for one download, never kept
   state.selected = getDownloadQuality();
   if (state.selected < 0 || state.selected >= DOWNLOAD_XMB) state.selected = DOWNLOAD_1440P;
   state.warning = 0;
   state.plateFill = interpolateColor(activeTheme->appBg, uiWithAlpha(activeTheme->hintFill, 0xFF),
                                      (float)(activeTheme->hintFill >> 24) / 255.0f);

   uiInitLabel(&titleLabel, UI_PAGE_TITLE_WEIGHT, CONTENT_X, 0, UI_SAFE_RIGHT - CONTENT_X, UI_PAGE_TITLE_SIZE,
               activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "Download");
   uiInitTextLabel(&videoLabel, UI_WEIGHT_SEMIBOLD, 0, 0, UI_SAFE_RIGHT - CONTENT_X, 24, activeTheme->textSecondary,
                   TEXT_NOWRAP_ELLIPSIS, pendingItem.title);
   const char *notes[DOWNLOAD_QUALITY_COUNT] = {
      "Plays in TEE Vanced (Downloads), with its Opus sound. A video without 1440p gets 1080p60.",
      "Plays in TEE Vanced (Downloads), on the TEE Decoder.",
      "Goes to the XMB's Video column, which plays it with the console's own player.",
   };
   for (int i = 0; i < DOWNLOAD_QUALITY_COUNT; i++) {
      uiInitLabel(&nameLabels[i], UI_WEIGHT_EXTRABOLD, 0, 0, LIST_W - 2 * ROW_PAD - ICON_SIZE - ICON_GAP, NAME_SIZE,
                  activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, CHOICE_NAMES[i]);
      uiInitLabel(&noteLabels[i], UI_WEIGHT_SEMIBOLD, 0, 0, LIST_W - 2 * ROW_PAD - ICON_SIZE - ICON_GAP, NOTE_SIZE,
                  activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, notes[i]);
   }
   initIcon(&rowIcon, ICON_DOWNLOAD, ICON_SIZE);
   uiInitLabel(&warningLabel, UI_BODY_WEIGHT, 0, 0, state.screenW * 2 / 3 - 88, 24, activeTheme->textPrimary, TEXT_WRAP,
               "For the XMB, the video goes to its Video column, and the system takes the file: TEE Vanced will not "
               "list or play it afterwards. The XMB plays it at 720p60 or 1080p30.");
   uiInitHints(&warningHints);
   uiAddHint(&warningHints, GLYPH_CROSS,  "Download for the XMB");
   uiAddHint(&warningHints, GLYPH_CIRCLE, "Back");
   uiInitHints(&hints);
   uiAddHint(&hints, GLYPH_CROSS,  "Download");
   uiAddHint(&hints, GLYPH_CIRCLE, "Cancel");
   memset(&navigateRepeat, 0, sizeof navigateRepeat);
}

static void updateDownloadChoice(void)
{
   if (state.warning) {   // modal: the XMB's warning owns the pad until it is answered
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { state.warning = 0; return; }
      if (isPadButtonPressed(PAD_BTN_CROSS)) {
         enqueueDownloadAs(&pendingItem, DOWNLOAD_XMB);   // not kept as the choice: the next R3 is the gallery again
         popScreen();
      }
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }
   if      (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN))) state.selected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP)))   state.selected--;
   if (state.selected < 0) state.selected = DOWNLOAD_QUALITY_COUNT - 1;
   if (state.selected >= DOWNLOAD_QUALITY_COUNT) state.selected = 0;
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (state.selected == DOWNLOAD_XMB) { state.warning = 1; return; }
      setDownloadQuality(state.selected);   // the next R3 starts on it (the gallery's two forms only)
      enqueueDownloadAs(&pendingItem, (DownloadQuality)state.selected);
      popScreen();   // the rail's download entry shows it from here on
   }
}

static void drawDownloadChoice(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->appBg);
   for (int i = 0; i < DOWNLOAD_QUALITY_COUNT; i++) {
      int y = LIST_TOP + i * ROW_H;
      int picked = i == state.selected;
      if (picked) {
         uiDrawFocusGlow(CONTENT_X, y, LIST_W, PLATE_H, activeTheme->focusGlow);
         uiFillRounded(CONTENT_X, y, LIST_W, PLATE_H, UI_RADIUS_ROW, state.plateFill);
         uiDrawFocusRing(CONTENT_X, y, LIST_W, PLATE_H, UI_RING_CARD, activeTheme->focusBorder);
      }
      if (rowIcon.tex)
         drawIcon(&rowIcon, CONTENT_X + ROW_PAD, y + (PLATE_H - rowIcon.tex->h) / 2 - rowIcon.offsetY,
                  picked ? activeTheme->accent : activeTheme->textSecondary);
      int textX = CONTENT_X + ROW_PAD + ICON_SIZE + ICON_GAP;
      uiDrawLabelAt(&nameLabels[i], textX, uiLabelYForBaseline(&nameLabels[i], y + 38));
      uiDrawLabelAt(&noteLabels[i], textX, uiLabelYForBaseline(&noteLabels[i], y + 68));
   }
   uiPlaceLabelCentered(&titleLabel, CONTENT_X, HEADER_Y, HEADER_H);
   drawLabel(&titleLabel);
   uiDrawLabelAt(&videoLabel, CONTENT_X, uiLabelYForBaseline(&videoLabel, SUBTITLE_BASE));
   uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);

   if (state.warning) {   // the box as the other screens draw theirs: scrim, panel, the ring of what has the pad
      int panelW = state.screenW * 2 / 3, padX = 44;
      int textTop = 40, hintsTop = textTop + warningLabel.tt.tex.h + 26;
      int panelH = hintsTop + UI_HINT_GLYPH_H + 30;
      int x = (state.screenW - panelW) / 2, y = (state.screenH - panelH) / 2;
      fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
      uiFillRounded(x, y, panelW, panelH, 26, activeTheme->surface);
      uiDrawFocusRing(x, y, panelW, panelH, UI_RING_ROW, activeTheme->focusBorder);
      uiDrawLabelAt(&warningLabel, x + padX, y + textTop);
      uiDrawHints(&warningHints, x + padX, y + hintsTop, UI_ALIGN_LEFT);
   }
}

static void termDownloadChoice(void)
{
   uiTermHints(&hints);
   uiTermHints(&warningHints);
   freeLabel(&warningLabel);
   freeIcon(&rowIcon);
   freeLabel(&titleLabel);
   freeLabel(&videoLabel);
   for (int i = 0; i < DOWNLOAD_QUALITY_COUNT; i++) { freeLabel(&nameLabels[i]); freeLabel(&noteLabels[i]); }
}

// Screen vtable order: init, resume, update, draw, suspend, term, status.
static Screen downloadChoiceScreen = { initDownloadChoice, NULL, updateDownloadChoice, drawDownloadChoice,
                                       NULL, termDownloadChoice, SCREEN_TERMINATED };

void openDownloadChoice(const SearchResult *item)
{
   if (!item || !item->videoId[0]) return;
   pendingItem = *item;
   pushScreen(&downloadChoiceScreen);
}

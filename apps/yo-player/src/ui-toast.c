// ui-toast - a short word at the bottom left of a list page (see ui-toast.h).

#include "ui-toast.h"
#include "i18n.h"   // (V3.1.0) tr: the texts in the language in use
#include "ui-draw.h"            // safe area, rounded fills, uiLabelYForCenter
#include "ui-fonts.h"           // the shared fonts and uiInitLabel
#include "ui-hints.h"           // UI_HINTS_BOTTOM_Y: the bottom hint row it stands above
#include "ui/icon-font.h"
#include "ui/label.h"
#include "theme.h"
#include <sys/sys_time.h>

#define TOAST_SHOW_US   2500000ull
#define TOAST_FADE_US   400000ull      // the last part of it: the word fades out
#define TOAST_X         176            // the page content's left edge, beside the collapsed rail
#define TOAST_H         84
#define TOAST_BOTTOM    (UI_HINTS_BOTTOM_Y - 28)   // clear of the bottom hint row
#define TOAST_PAD_X     30
#define TOAST_EDGE      3              // the accent edge round the plate
#define TOAST_RADIUS    UI_RADIUS_PANEL
#define TOAST_ICON_SIZE 40
#define TOAST_ICON_GAP  18
#define TOAST_TEXT_SIZE 34
#define TOAST_WEIGHT    UI_WEIGHT_BOLD

static Label    label;
static int      labelMade;
static Icon     icon;
static int      iconMade;
static uint64_t shownUs;      // 0 = no word standing

void uiShowToast(IconId id, const char *text)
{
   // (V3.1.0) the font picked for every word: the label outlives a change of the language (Japanese needs FONT_POP)
   if (!labelMade) {
      uiInitTextLabel(&label, TOAST_WEIGHT, 0, 0, AUTO, TOAST_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, text);
      labelMade = 1;
   } else {
      uiSetTextLabel(&label, TOAST_WEIGHT, text);
   }
   if (!iconMade || icon.id != id) {
      initIcon(&icon, id, TOAST_ICON_SIZE);
      iconMade = 1;
   }
   shownUs = sys_time_get_system_time();
   if (!shownUs) shownUs = 1;
}

// a colour at `alpha` of its own alpha
static uint32_t faded(uint32_t argb, int alpha)
{
   uint32_t a = ((argb >> 24) * (uint32_t)alpha) / 255u;
   return (a << 24) | (argb & 0x00FFFFFFu);
}

void uiDrawToast(void)
{
   if (!shownUs || !labelMade || label.tt.tex.w <= 0) return;
   uint64_t age = sys_time_get_system_time() - shownUs;
   if (age >= TOAST_SHOW_US) { shownUs = 0; return; }
   int alpha = 255;
   if (age > TOAST_SHOW_US - TOAST_FADE_US) alpha = (int)(255 * (TOAST_SHOW_US - age) / TOAST_FADE_US);

   int textW = label.tt.tex.w;
   int w = TOAST_PAD_X + TOAST_ICON_SIZE + TOAST_ICON_GAP + textW + TOAST_PAD_X;
   int y = TOAST_BOTTOM - TOAST_H;
   uiFillRounded(TOAST_X, y, w, TOAST_H, TOAST_RADIUS, faded(activeTheme->accent, alpha));
   uiFillRounded(TOAST_X + TOAST_EDGE, y + TOAST_EDGE, w - 2 * TOAST_EDGE, TOAST_H - 2 * TOAST_EDGE,
                 TOAST_RADIUS - TOAST_EDGE, faded(activeTheme->surface1 | 0xFF000000u, alpha));
   if (iconMade)
      drawIconAlpha(&icon, TOAST_X + TOAST_PAD_X, y + (TOAST_H - TOAST_ICON_SIZE) / 2, activeTheme->accent, alpha);
   moveLabel(&label, TOAST_X + TOAST_PAD_X + TOAST_ICON_SIZE + TOAST_ICON_GAP, uiLabelYForCenter(&label, y, TOAST_H));
   drawLabelAlpha(&label, alpha * (int)(label.color >> 24) / 255);
}

void uiShowWatchLaterToast(int wasIn, int isIn)
{
   uiShowToast(ICON_CLOCK, isIn ? tr("Added to Watch Later") : wasIn ? tr("Removed from Watch Later") : tr("Watch Later is full"));
}

void uiTermToast(void)
{
   if (labelMade) freeLabel(&label);
   labelMade = 0;
   if (iconMade) freeIcon(&icon);
   iconMade = 0;
   shownUs = 0;
}

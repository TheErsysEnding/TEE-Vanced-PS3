// sidebar - implementation (see sidebar.h). Modelled on apps/swarm's chrome.c, which already drives a left
// icon rail with this exact focus model.

#include "sidebar.h"

#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "ui-fonts.h"         // uiInitLabel / uiLabelYForCenter: the entry names are Manrope like all UI text
#include "ui-draw.h"          // rounded fills, uiWithAlpha, UI_SCREEN_W
#include "account.h"          // signed in or not: the account entry's name and icon
#include <string.h>

// Ember's rail at 1920x1080. Collapsed (Bild 1 and 3) it is a 112-px column with no surface of its own: the
// orange logo tile, then one 64x64 slot per entry, 14 apart, and Settings alone at the foot.
#define LOGO_X         26
#define LOGO_Y         48         // the top of the safe area
#define LOGO_SIZE      60
#define LOGO_RADIUS    18
#define LOGO_TEXT      19         // "TEE" 19/800 in on-accent
#define WORDMARK_GAP   14         // expanded: the tile -> "Vanced" 26/800, as in Bild 4's logo row
#define WORDMARK_TEXT  26
#define SLOT_X         24
#define SLOT_SIZE      64
#define SLOT_RADIUS    20
#define SLOT_TOP       158        // the logo's 48 + 60, its 36 below it, and the column's 14 gap
#define SLOT_STEP      72         // 64 + 8: eleven entries, two at the foot - at Bild 1's 14 the ninth of the
                                  // stack (History) ran into the account entry
#define SLOT_FOOT      54         // Settings' slot ends 54 above the bottom edge (Bild 1 and 3)
#define ICON_SIZE      30
#define ICON_CENTER_X  (SLOT_X + SLOT_SIZE / 2)
#define ICON_REST_ALPHA 0x9E      // a resting icon in the collapsed rail: text-1 at 62 %, as Bild 1 and 3 draw it
// Expanded (Bild 4 and 5) it is a panel over the content: a row per entry at the SAME height as its slot,
// so nothing moves when it opens, with the name 18 px after the icon.
#define PANEL_MIN_W    300
#define ROW_X          20         // a row runs from 20 in to 20 short of the panel's edge
#define ROW_RADIUS     18
#define ROW_PAD_R      18         // the name -> the row's right end
#define LABEL_X        (ICON_CENTER_X + ICON_SIZE / 2 + 18)
#define LABEL_SIZE     24         // Ember's panel entries: 24/700
#define LABEL_WEIGHT   UI_WEIGHT_BOLD
#define EDGE_W         2          // the panel's right edge, a hairline against the dimmed content
#define DISABLED_ALPHA 110
// The names are rendered once in white and tinted when drawn, the way the icons beside them are: the tint
// follows the highlight, and re-colouring a rendered label means rasterising it again - which setLabelColor
// used to do in the draw path on every move of the highlight.
#define TEXT_TINTABLE 0xFFFFFFFFu

// Four of the eleven have an exact glyph in the embedded set (search, clock, history, cog). The other seven
// have none - there is no home, play, trending, broadcast, subscription, playlist or person symbol in it - so they
// borrow the nearest honest reading rather than a made-up one: a pointing hand for what was picked out for you, an
// up arrow for what is rising, the signal arcs for what is on air, a star for channels you follow, a right arrow
// for picking a video back up, stacked pages for lists of videos, and a lock for the sign-in - closed for "Login",
// open once signed in ("Account").
static const IconId ITEM_ICONS[SIDE_COUNT] = {
   ICON_SEARCH,      // Search
   ICON_HAND_POINTER_O,   // For you
   ICON_UP_BIG,      // Trending
   ICON_WIFI,        // Live
   ICON_STAR,        // Subscriptions
   ICON_RIGHT_BIG,   // Continue watching
   ICON_DOCS,        // Playlists
   ICON_CLOCK,       // Watch Later
   ICON_HISTORY,     // History
   ICON_LOCK,        // Login (signed in: accountIcon, the open lock)
   ICON_COG,         // Settings
};

static const char *ITEM_NAMES[SIDE_COUNT] = {
   "Search", "For you", "Trending", "Live", "Subscriptions", "Continue watching",
   "Playlists", "Watch Later", "History", "Login", "Settings",
};

static Icon  icons[SIDE_COUNT];
static Label labels[SIDE_COUNT];
static Icon  accountIcon;      // SIDE_ACCOUNT while signed in: the open lock ...
static Label accountLabel;     // ... and "Account"
static Label logoLabel, wordmarkLabel;
static int   current = SIDE_SUBS;   // what the content is showing
static int   picked  = SIDE_SUBS;   // where the highlight sits while focused
static int   focused;
static int   panelWidth = PANEL_MIN_W;   // PANEL_MIN_W, or wider when the longest name needs it

int isSidebarItemEnabled(SidebarItem item) { (void)item; return 1; }   // every entry leads somewhere now
const char *sidebarItemName(SidebarItem item)
{
   if (item == SIDE_ACCOUNT && isAccountSignedIn()) return "Account";
   return (item >= 0 && item < SIDE_COUNT) ? ITEM_NAMES[item] : "";
}

// the account entry follows the sign-in; every other entry is what it always is
static Icon  *entryIcon(int index)  { return index == SIDE_ACCOUNT && isAccountSignedIn() ? &accountIcon : &icons[index]; }
static Label *entryLabel(int index) { return index == SIDE_ACCOUNT && isAccountSignedIn() ? &accountLabel : &labels[index]; }

void initSidebar(void)
{
   focused = 0;
   current = picked = SIDE_SUBS;
   panelWidth = PANEL_MIN_W;

   uiInitLabel(&logoLabel, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, LOGO_TEXT, activeTheme->onAccent, TEXT_NOWRAP, "TEE");
   uiInitLabel(&wordmarkLabel, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, WORDMARK_TEXT, activeTheme->textPrimary, TEXT_NOWRAP, "Vanced");
   for (int i = 0; i < SIDE_COUNT; i++) {
      initIcon(&icons[i], ITEM_ICONS[i], ICON_SIZE);
      uiInitLabel(&labels[i], LABEL_WEIGHT, 0, 0, AUTO, LABEL_SIZE, TEXT_TINTABLE, TEXT_NOWRAP, ITEM_NAMES[i]);
      // The panel takes its longest name whole: "Continue watching" at 24/700 needs about 350 px where the
      // design's panel is 300, and an ellipsis in a menu of nine fixed words would only hide which entry it is.
      int needed = LABEL_X + labels[i].tt.tex.w + ROW_PAD_R + ROW_X;
      if (needed > panelWidth) panelWidth = needed;
   }
   initIcon(&accountIcon, ICON_LOCK_OPEN, ICON_SIZE);
   uiInitLabel(&accountLabel, LABEL_WEIGHT, 0, 0, AUTO, LABEL_SIZE, TEXT_TINTABLE, TEXT_NOWRAP, "Account");
}

void termSidebar(void)
{
   for (int i = 0; i < SIDE_COUNT; i++) { freeIcon(&icons[i]); freeLabel(&labels[i]); }
   freeIcon(&accountIcon);
   freeLabel(&accountLabel);
   freeLabel(&logoLabel);
   freeLabel(&wordmarkLabel);
}

void setSidebarCurrent(SidebarItem item)
{
   if (item < 0 || item >= SIDE_COUNT) return;
   current = item;
   if (!focused) picked = item;   // while unfocused the highlight simply follows what is on screen
}

int isSidebarFocused(void) { return focused; }

void setSidebarFocused(int focus)
{
   focused = focus ? 1 : 0;
   if (focused) picked = current;   // always open on the entry you are actually looking at
}

static int step(int from, int delta)
{
   int next = from + delta;
   if (next < 0) next = SIDE_COUNT - 1;
   if (next >= SIDE_COUNT) next = 0;
   return next;
}

int updateSidebar(void)
{
   if (!focused) return -1;

   if (isPadButtonPressed(PAD_BTN_DOWN)) picked = step(picked, 1);
   if (isPadButtonPressed(PAD_BTN_UP))   picked = step(picked, -1);

   // O and RIGHT leave the rail without changing anything: RIGHT points back at the page, and you land where you
   // were on it. Only X commits the highlighted entry - RIGHT used to as well, so a RIGHT meant as "back" opened
   // whatever the highlight had been moved to.
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_RIGHT)) { focused = 0; return -1; }

   if (isPadButtonPressed(PAD_BTN_CROSS)) return picked;
   return -1;
}

// the top of an entry's slot - the same in both states, so the icons stay put when the panel opens. Settings
// is pinned to the foot of the rail and the account entry sits on top of it; the rest stack under the logo.
static int slotY(int index, int screenH)
{
   if (index == SIDE_COUNT - 1) return screenH - SLOT_FOOT - SLOT_SIZE;
   if (index == SIDE_ACCOUNT)   return screenH - SLOT_FOOT - SLOT_SIZE - SLOT_STEP;
   return SLOT_TOP + index * SLOT_STEP;
}

// a name (rendered in TEXT_TINTABLE) drawn in `color`, alpha included
static void drawTinted(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, color,
                  GFX_FILTER_NEAREST);
}

// An entry's icon with its ink centred in the slot both ways: drawIcon places a glyph by its em box, and the
// glyph does not fill that box, so the box's middle is not the icon's.
static void drawEntryIcon(int index, int slotTop, uint32_t color)
{
   Icon *icon = entryIcon(index);
   if (!icon->tex || icon->tex->h <= 0) return;
   if (!isSidebarItemEnabled((SidebarItem)index)) color = uiWithAlpha(color, DISABLED_ALPHA);
   drawIconCentered(icon, ICON_CENTER_X, slotTop + (SLOT_SIZE - icon->tex->h) / 2 - icon->offsetY, color);
}

static void drawLogo(int expanded)
{
   uiFillRounded(LOGO_X, LOGO_Y, LOGO_SIZE, LOGO_SIZE, LOGO_RADIUS, activeTheme->accent);
   uiDrawLabelAt(&logoLabel, LOGO_X + (LOGO_SIZE - logoLabel.tt.tex.w) / 2, uiLabelYForCenter(&logoLabel, LOGO_Y, LOGO_SIZE));
   if (expanded)
      uiDrawLabelAt(&wordmarkLabel, LOGO_X + LOGO_SIZE + WORDMARK_GAP, uiLabelYForCenter(&wordmarkLabel, LOGO_Y, LOGO_SIZE));
}

// The collapsed rail: no surface of its own, just the logo and the icons on the page background, with
// `active` on its accent-soft square in the accent colour and the others at 62 % of text-1.
static void drawCollapsed(int screenH, int active)
{
   drawLogo(0);
   for (int i = 0; i < SIDE_COUNT; i++) {
      int y = slotY(i, screenH);
      if (i == active) uiFillRounded(SLOT_X, y, SLOT_SIZE, SLOT_SIZE, SLOT_RADIUS, activeTheme->accentSoft);
      drawEntryIcon(i, y, i == active ? activeTheme->accent : uiWithAlpha(activeTheme->textPrimary, ICON_REST_ALPHA));
   }
}

// The expanded panel. The highlight (where X would go) is the accent-soft row with an orange icon and its
// name in accent-text; the entry the content belongs to keeps its orange icon and reads in text-1 while the
// highlight is elsewhere, so both stay visible at once; the rest are text-2.
static void drawExpanded(int screenH)
{
   uiFillRect(panelWidth, 0, UI_SCREEN_W - panelWidth, screenH, activeTheme->scrim);
   uiFillRect(0, 0, panelWidth, screenH, activeTheme->surface1);
   uiFillRect(panelWidth - EDGE_W, 0, EDGE_W, screenH, activeTheme->hairline);
   drawLogo(1);

   for (int i = 0; i < SIDE_COUNT; i++) {
      int y = slotY(i, screenH);
      int isPicked  = (i == picked);
      int isCurrent = (i == current);
      if (isPicked) uiFillRounded(ROW_X, y, panelWidth - 2 * ROW_X, SLOT_SIZE, ROW_RADIUS, activeTheme->accentSoft);
      uint32_t iconColor = (isPicked || isCurrent) ? activeTheme->accent : activeTheme->textSecondary;
      uint32_t nameColor = isPicked ? activeTheme->accentText : isCurrent ? activeTheme->textPrimary : activeTheme->textSecondary;
      if (!isSidebarItemEnabled((SidebarItem)i)) nameColor = uiWithAlpha(nameColor, DISABLED_ALPHA);
      drawEntryIcon(i, y, iconColor);
      const Label *name = entryLabel(i);
      drawTinted(name, LABEL_X, uiLabelYForCenter(name, y, SLOT_SIZE), nameColor);
   }
}

void drawSidebar(int screenH)
{
   // The panel runs the full height, Settings at its foot as in the rail. It used to stop above the
   // button-hint row: that row was centred, wide enough to reach under the panel, and painting over it made
   // the hints look broken. The hint rows are right-aligned now and start far to the right of the panel.
   if (focused) drawExpanded(screenH);
   else         drawCollapsed(screenH, current);
}

void drawSidebarPassive(int screenH, SidebarItem active)
{
   drawCollapsed(screenH, (active >= 0 && active < SIDE_COUNT) ? (int)active : -1);
}

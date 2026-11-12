// home-list - Home's list view (see home-list.h).
//
// Laid out on the 1920 x 1080 canvas: the rows from the rail's edge (176) to 936, LIST_VISIBLE of them under the
// header; the details right of them to the safe edge; the selection's picture where the Home preview has it
// (520, 0, 1400 x 788), darker towards the rows and fading into the page at its bottom. Every label here is white
// and tinted when drawn, so one alpha fades the whole layer in and out (layerAlpha) and a row needs no second
// rasterisation for its focused look. The rows' labels live in ROW_SLOTS slots by index, re-rendered only when
// the slot's video or its quality changes.

#include "screens/home-list.h"
#include "video-details.h"
#include "channel-avatars.h"
#include "account.h"            // isAccountSignedIn / isAccountFollowing: the Subscribe button's state
#include "storage.h"            // isSubscribed (signed out)
#include "ui-fonts.h"
#include "ui-draw.h"
#include "gfx.h"
#include "theme.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "anim.h"               // applyEasing
#include "button-repeat.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"
#include <sys/sys_time.h>
#include <string.h>
#include <stdio.h>

// ---- the rows ----
#define LIST_X           176
#define LIST_W           760
#define LIST_TOP         150
#define ROW_STEP         118
#define ROW_H            106
#define LIST_VISIBLE        7
#define ROW_SLOTS        (LIST_VISIBLE + 2)
#define ROW_RADIUS       22
#define ROW_THUMB_X      10
#define ROW_THUMB_Y      8
#define ROW_THUMB_W      160
#define ROW_THUMB_H      90
#define ROW_THUMB_RADIUS 12
#define ROW_TEXT_X       (ROW_THUMB_X + ROW_THUMB_W + 22)
#define ROW_TEXT_W       (LIST_W - ROW_TEXT_X - 18)
#define ROW_TITLE_SIZE   26
#define ROW_TITLE_BASE   35
#define ROW_QUALITY_SIZE 22
#define ROW_QUALITY_BASE 65
#define ROW_META_SIZE    20
#define ROW_META_BASE    93
#define ROW_AVATAR       24
#define ROW_AVATAR_GAP   8

// ---- the details ----
#define PANEL_X          1000
#define PANEL_W          (UI_SAFE_RIGHT - PANEL_X)
#define PANEL_TITLE_SIZE 40
#define PANEL_TITLE_BASE 196
#define PANEL_META_SIZE  24
#define PANEL_CHIP_H     42
#define PANEL_CHIP_PAD   16
#define PANEL_CHIP_SIZE  22
#define PANEL_CHIP_GAP   12
#define PANEL_NOTE_SIZE  22
#define PANEL_DESC_SIZE  22
#define BAR_Y            856
#define BAR_H            116
#define BAR_PAD          22
#define BAR_AVATAR       72
#define BUTTON_H         54
#define BUTTON_PAD       26
#define BUTTON_SIZE      22
#define BUTTON_GAP       14
#define CHANNEL_NAME_W   (PANEL_W - 2 * BAR_PAD - BAR_AVATAR - 20 - 400)   // the two buttons take up to 400

// ---- the picture behind ----
#define PICTURE_X        520
#define PICTURE_W        1400
#define PICTURE_H        788
#define PICTURE_FADE_TOP 420

#define OPEN_US          260000    // the layer eases in (and out) over this long
#define SLIDE_PX         90        // ... rising this far
#define DETAILS_REST_US  250000    // the selection rests this long before its video is resolved
#define TINTABLE         0xFFFFFFFFu
#define DOT              " \xC2\xB7 "

typedef struct {
   int   index;           // the result it holds, -1 none
   char  videoId[16];
   char  quality[40];     // what its quality line says
   int   indent;          // its channel line starts after the channel's picture
   Label title, quality_, meta;
} RowSlot;

static RowSlot      rows[ROW_SLOTS];
static Label        panelTitle, panelMeta, qualityChip, durationChip, liveChip, subsChip, audioChip, rewindChip;
static Label        checkingLabel, noteLabel, channelName, channelState, channelButton, subscribeButton;
static TextTexture  descTex;
static Font        *descFont;
static char         panelVideoId[16];
static int          panelDetails;        // the details shown are the resolve's (not just the feed's)
static int          panelFollowing = -1;
static char         panelChannelId[32];

static int          target;              // 1 open (or opening), 0 closed (or closing)
static float        animFrom;
static uint64_t     animSinceUs;
static int          top;                 // the first row on screen
static int          focusButtons, button;   // the details' buttons have the focus; which one (0 Channel, 1 Subscribe)
static int          lastSelected = -1;
static uint64_t     selectedSinceUs;
static ButtonRepeat verticalRepeat, pageRepeat;
static int          layerAlpha = 255;    // this frame's
static int          skipInput;           // the frame that opened it: its DOWN must not move the selection too
static int          initialised;

// ---- helpers ----

static float openness(void)
{
   uint64_t elapsed = sys_time_get_system_time() - animSinceUs;
   float to = target ? 1.0f : 0.0f;
   if (elapsed >= OPEN_US) return to;
   float t = applyEasing((float)elapsed / (float)OPEN_US, EASE_OUT_CUBIC);
   return animFrom + (to - animFrom) * t;
}

static void setTarget(int open, int animated)
{
   float now = openness();
   target = open;
   animFrom = animated ? now : (open ? 1.0f : 0.0f);
   animSinceUs = sys_time_get_system_time() - (animated ? 0 : OPEN_US);
}

static uint32_t fade(uint32_t argb) { return uiWithAlpha(argb, (int)(((argb >> 24) * (uint32_t)layerAlpha + 127) / 255)); }

static void drawTint(const Label *label, int x, int y, uint32_t color)
{
   if (label->tt.tex.w <= 0) return;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, fade(color),
                  GFX_FILTER_NEAREST);
}

static void drawAtBaseline(const Label *label, int x, int baseline, uint32_t color)
{
   drawTint(label, x, uiLabelYForBaseline(label, baseline), color);
}

// a wrapped label's last baseline, its first at `baseline`
static int lastBaseline(const Label *label, int baseline)
{
   UiTextMetrics m = uiGetTextMetrics(label->font, label->size);
   int extra = label->tt.tex.h - m.texH;
   return baseline + (extra > 0 ? extra : 0);
}

// a chip: the label on a pill of `fill`, padX either side. Returns its width (0 for an empty label).
static int drawChip(int x, int y, int h, int padX, const Label *label, uint32_t fill, uint32_t text)
{
   if (label->tt.tex.w <= 0) return 0;
   int w = label->tt.tex.w + 2 * padX;
   uiFillPill(x, y, w, h, fade(fill));
   drawTint(label, x + padX, uiLabelYForCenter(label, y, h), text);
   return w;
}

static void drawAvatar(const char *channelId, int x, int y, int d, uint32_t behind)
{
   const GfxTexture *avatar = getChannelAvatarTexture(channelId);
   if (avatar) {
      drawGfxTexture(x, y, d, d, *avatar, 0.0f, 0.0f, 1.0f, 1.0f, fade(0xFFFFFFFF), GFX_FILTER_LINEAR);
      uiDrawImageCorners(x, y, d, d, d / 2, behind);   // a circle (the corners in the colour behind)
   } else {
      uiFillCircle(x, y, d, fade(activeTheme->surface1));
   }
}

// a feed item's title as the cards show it: one that would come out blank (Thai, Arabic) gives its channel's name
static void itemTitle(const SearchResult *item, char *out, int cap)
{
   uiCleanText(out, cap, item->title);
   if (uiTextIsLegible(out)) return;
   uiCleanText(out, cap, item->author);
}

static int isFollowing(const char *channelId)
{
   if (!channelId || !channelId[0]) return 0;
   return isAccountSignedIn() ? isAccountFollowing(channelId) : isSubscribed(channelId);
}

// the quality line of a row: "1440p60", "LIVE · 1080p60", or what is known of it so far
static void rowQuality(const SearchResult *item, char *out, int cap)
{
   const VideoDetails *details = findVideoDetails(item->videoId);
   char quality[16] = "";
   if (details && !details->failed) formatVideoQuality(quality, sizeof quality, details->bestHeight, details->bestFps);
   if (item->isLive) snprintf(out, cap, quality[0] ? "LIVE" DOT "%s" : "LIVE", quality);
   else if (quality[0]) snprintf(out, cap, "%s", quality);
   else if (details) snprintf(out, cap, "%s", details->failed ? "Quality not known" : "Nothing it can play");
   else snprintf(out, cap, "...");
}

// ---- the rows' labels ----

static void labelRow(const SearchResults *results, int index)
{
   RowSlot *slot = &rows[index % ROW_SLOTS];
   const SearchResult *item = &results->items[index];
   if (slot->index != index || strcmp(slot->videoId, item->videoId) != 0) {
      slot->index = index;
      strCopy(slot->videoId, sizeof slot->videoId, item->videoId);
      char title[sizeof item->title];
      itemTitle(item, title, sizeof title);
      uiSetTextLabel(&slot->title, UI_WEIGHT_BOLD, title);
      char meta[160];
      const char *second = item->isLive ? item->views : item->duration;
      snprintf(meta, sizeof meta, "%s%s%s%s%s", item->author, item->author[0] && second[0] ? DOT : "", second,
               item->published[0] ? DOT : "", item->published);
      slot->indent = hasChannelAvatar(item->channelId);
      freeLabel(&slot->meta);   // a new width needs a new texture: setLabelText skips a text it already shows
      slot->meta.width = ROW_TEXT_W - (slot->indent ? ROW_AVATAR + ROW_AVATAR_GAP : 0);
      uiSetTextLabel(&slot->meta, UI_WEIGHT_SEMIBOLD, meta);
      slot->quality[0] = 0;
   }
   char quality[40];
   rowQuality(item, quality, sizeof quality);
   if (strcmp(quality, slot->quality) != 0) {
      strCopy(slot->quality, sizeof slot->quality, quality);
      setLabelText(&slot->quality_, quality);
   }
}

// ---- the details' labels ----

static void labelPanel(const SearchResult *item)
{
   const VideoDetails *details = findVideoDetails(item->videoId);
   int changed = strcmp(panelVideoId, item->videoId) != 0;
   if (changed) {
      strCopy(panelVideoId, sizeof panelVideoId, item->videoId);
      char title[sizeof item->title];
      itemTitle(item, title, sizeof title);
      uiSetTextLabel(&panelTitle, UI_WEIGHT_EXTRABOLD, title);
      char meta[160];
      snprintf(meta, sizeof meta, "%s%s%s%s%s", item->author, item->author[0] && item->views[0] ? DOT : "", item->views,
               item->published[0] ? DOT : "", item->published);
      uiSetTextLabel(&panelMeta, UI_WEIGHT_SEMIBOLD, meta);
      uiSetTextLabel(&durationChip, UI_WEIGHT_BOLD, item->isLive ? "" : item->duration);
      setLabelText(&liveChip, item->isLive ? "LIVE" : "");
      uiSetTextLabel(&channelName, UI_WEIGHT_BOLD, item->author[0] ? item->author : "");
      strCopy(panelChannelId, sizeof panelChannelId, item->channelId);
      panelFollowing = -1;
      panelDetails = -1;
   }

   int have = details != NULL;
   if (have != panelDetails || changed) {
      panelDetails = have;
      freeTextTexture(&descTex);
      char quality[16] = "", note[200] = "";
      if (details && !details->failed) {
         formatVideoQuality(quality, sizeof quality, details->bestHeight, details->bestFps);
         char playing[16];
         formatVideoQuality(playing, sizeof playing, details->playHeight, details->playFps);
         if (!quality[0])
            snprintf(note, sizeof note, "The PS3 has no stream of it that it can play.");
         else if (playing[0] && strcmp(playing, quality) != 0 && details->qualityCap == 0)
            snprintf(note, sizeof note, "Auto plays it at %s - choose %dp in the player's options (START) for its "
                     "best.", playing, details->bestHeight);
         else if (playing[0] && strcmp(playing, quality) != 0)
            snprintf(note, sizeof note, "Plays at %s with your quality setting (%dp).", playing, details->qualityCap);
         else if (details->bestHeight >= 1440 && details->bestFps >= 50)
            snprintf(note, sizeof note, "The best the PS3 plays. In its busiest moments it can drop to 1080p60 for a "
                     "few seconds.");
         else
            snprintf(note, sizeof note, "The best the PS3 plays of it.");
         char text[16];
         snprintf(text, sizeof text, "%d audio tracks", details->audioTracks);
         setLabelText(&audioChip, details->audioTracks > 1 ? text : "");
         setLabelText(&subsChip, details->captions > 0 ? "Subtitles" : "");
         setLabelText(&rewindChip, details->isLive && details->liveDvr ? "Rewind" : "");

         // the start of the description, wrapped to the panel; the draw shows as many whole lines as there is room
         static char description[DETAILS_DESCRIPTION_CAP + 8];
         strCopy(description, sizeof description, details->description);
         if (details->descriptionCut) strCopy(description + strlen(description), 8, "...");
         uiCleanText(description, (int)sizeof description, description);
         if (description[0]) {
            descFont = uiPickFont(UI_BODY_WEIGHT, description);
            renderFontRaw(&descTex, descFont, PANEL_DESC_SIZE, description, TINTABLE, PANEL_W, TEXT_WRAP);
         }
      } else {
         setLabelText(&audioChip, "");
         setLabelText(&subsChip, "");
         setLabelText(&rewindChip, "");
         if (details) snprintf(note, sizeof note, "Its details could not be fetched.");
      }
      setLabelText(&qualityChip, quality);
      setLabelText(&noteLabel, note);
   }

   int following = isFollowing(item->channelId);
   if (following != panelFollowing) {
      panelFollowing = following;
      setLabelText(&subscribeButton, following ? "Unsubscribe" : "Subscribe");
      setLabelText(&channelState, following ? "Subscribed" : "");
   }
}

// ---- open, close ----

void initHomeList(void)
{
   for (int s = 0; s < ROW_SLOTS; s++) {
      rows[s].index = -1;
      uiInitTextLabel(&rows[s].title, UI_WEIGHT_BOLD, 0, 0, ROW_TEXT_W, ROW_TITLE_SIZE, TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
      uiInitLabel(&rows[s].quality_, UI_WEIGHT_BOLD, 0, 0, ROW_TEXT_W, ROW_QUALITY_SIZE, TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
      uiInitTextLabel(&rows[s].meta, UI_WEIGHT_SEMIBOLD, 0, 0, ROW_TEXT_W, ROW_META_SIZE, TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
   }
   uiInitTextLabel(&panelTitle, UI_WEIGHT_EXTRABOLD, 0, 0, PANEL_W, PANEL_TITLE_SIZE, TINTABLE, TEXT_WRAP, "");
   uiInitTextLabel(&panelMeta, UI_WEIGHT_SEMIBOLD, 0, 0, PANEL_W, PANEL_META_SIZE, TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&qualityChip, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, PANEL_CHIP_SIZE + 2, TINTABLE, TEXT_NOWRAP, "");
   uiInitTextLabel(&durationChip, UI_WEIGHT_BOLD, 0, 0, AUTO, PANEL_CHIP_SIZE, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&liveChip, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, PANEL_CHIP_SIZE, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&subsChip, UI_WEIGHT_BOLD, 0, 0, AUTO, PANEL_CHIP_SIZE, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&audioChip, UI_WEIGHT_BOLD, 0, 0, AUTO, PANEL_CHIP_SIZE, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&rewindChip, UI_WEIGHT_BOLD, 0, 0, AUTO, PANEL_CHIP_SIZE, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&checkingLabel, UI_WEIGHT_SEMIBOLD, 0, 0, AUTO, PANEL_NOTE_SIZE, TINTABLE, TEXT_NOWRAP, "Checking the quality...");
   uiInitLabel(&noteLabel, UI_WEIGHT_SEMIBOLD, 0, 0, PANEL_W, PANEL_NOTE_SIZE, TINTABLE, TEXT_WRAP, "");
   uiInitTextLabel(&channelName, UI_WEIGHT_BOLD, 0, 0, CHANNEL_NAME_W, 26, TINTABLE, TEXT_NOWRAP_ELLIPSIS, "");
   uiInitLabel(&channelState, UI_WEIGHT_SEMIBOLD, 0, 0, AUTO, 20, TINTABLE, TEXT_NOWRAP, "");
   uiInitLabel(&channelButton, UI_WEIGHT_BOLD, 0, 0, AUTO, BUTTON_SIZE, TINTABLE, TEXT_NOWRAP, "Channel");
   uiInitLabel(&subscribeButton, UI_WEIGHT_BOLD, 0, 0, AUTO, BUTTON_SIZE, TINTABLE, TEXT_NOWRAP, "Subscribe");
   memset(&descTex, 0, sizeof descTex);
   panelVideoId[0] = 0;
   target = 0;
   animFrom = 0.0f;
   animSinceUs = 0;
   initialised = 1;
}

void termHomeList(void)
{
   if (!initialised) return;
   stopHomeList();
   for (int s = 0; s < ROW_SLOTS; s++) { freeLabel(&rows[s].title); freeLabel(&rows[s].quality_); freeLabel(&rows[s].meta); }
   freeLabel(&panelTitle); freeLabel(&panelMeta); freeLabel(&qualityChip); freeLabel(&durationChip);
   freeLabel(&liveChip); freeLabel(&subsChip); freeLabel(&audioChip); freeLabel(&rewindChip);
   freeLabel(&checkingLabel); freeLabel(&noteLabel); freeLabel(&channelName); freeLabel(&channelState);
   freeLabel(&channelButton); freeLabel(&subscribeButton);
   freeTextTexture(&descTex);
   initialised = 0;
}

static void keepInView(int selected, int count)
{
   if (selected > top + LIST_VISIBLE - 2) top = selected - (LIST_VISIBLE - 2);
   if (selected < top + 1) top = selected - 1;
   if (top > count - LIST_VISIBLE) top = count - LIST_VISIBLE;
   if (top < 0) top = 0;
}

void openHomeList(VideoGrid *grid)
{
   const SearchResults *results = gridResults(grid);
   int selected = gridSelectedIndex(grid);
   top = selected - 1;
   keepInView(selected, results->count);
   focusButtons = 0;
   button = 0;
   lastSelected = -1;
   resumeHomeList();
   skipInput = 1;
   setTarget(1, 1);
   logInfo("[home] list view open at %d of %d\n", selected + 1, results->count);
}

void closeHomeList(int animated)
{
   if (!target && !animated) { setTarget(0, 0); return; }
   setTarget(0, animated);
   focusButtons = 0;
}

int isHomeListOpen(void)         { return target; }
int isHomeListShown(void)        { return target || openness() > 0.0f; }
int isHomeListCovering(void)     { return target && openness() >= 1.0f; }
int homeListButtonsFocused(void) { return target && focusButtons; }
int homeListSubscribeFocused(void) { return target && focusButtons && button == 1; }

void resumeHomeList(void)
{
   uint64_t now = sys_time_get_system_time();
   verticalRepeat.timer = now; verticalRepeat.repeats = 0;
   pageRepeat.timer = now;     pageRepeat.repeats = 0;
}

void reapHomeList(void) { if (!target) updateVideoDetails(NULL, 0); }

void stopHomeList(void)
{
   stopVideoDetails();
   stopChannelAvatars();
}

// ---- input ----

HomeListAction updateHomeList(VideoGrid *grid)
{
   if (!target) return HOME_LIST_PASS;
   if (skipInput) { skipInput = 0; return HOME_LIST_HANDLED; }
   const SearchResults *results = gridResults(grid);
   int count = results->count, selected = gridSelectedIndex(grid);
   if (count <= 0) return HOME_LIST_HANDLED;
   const SearchResult *item = &results->items[selected];
   int hasChannel = item->channelId[0] != 0;
   if (!hasChannel) focusButtons = 0;

   int moved = 0;
   if (isRepeatDue(&verticalRepeat, getPadButtonState(PAD_BTN_DOWN))) {
      if (selected + 1 < count) { selected++; moved = 1; }
   } else if (isRepeatDue(&verticalRepeat, getPadButtonState(PAD_BTN_UP))) {
      if (selected == 0) { closeHomeList(1); return HOME_LIST_HANDLED; }   // UP off the first video: the cards
      selected--; moved = 1;
   }
   if (isRepeatDue(&pageRepeat, getPadButtonState(PAD_BTN_R2))) {
      int to = selected + LIST_VISIBLE < count ? selected + LIST_VISIBLE : count - 1;
      if (to != selected) { selected = to; moved = 1; }
   } else if (isRepeatDue(&pageRepeat, getPadButtonState(PAD_BTN_L2))) {
      int to = selected - LIST_VISIBLE > 0 ? selected - LIST_VISIBLE : 0;
      if (to != selected) { selected = to; moved = 1; }
   }
   if (moved) {
      setGridSelection(grid, selected);
      selected = gridSelectedIndex(grid);
   }
   keepInView(selected, count);

   if (isPadButtonPressed(PAD_BTN_RIGHT) && hasChannel) {
      if (!focusButtons) { focusButtons = 1; button = 0; }
      else button = 1;
      return HOME_LIST_HANDLED;
   }
   if (isPadButtonPressed(PAD_BTN_LEFT) && focusButtons) {
      if (button == 1) button = 0;
      else focusButtons = 0;
      return HOME_LIST_HANDLED;
   }
   if (focusButtons && isPadButtonPressed(PAD_BTN_CROSS)) return button == 0 ? HOME_LIST_CHANNEL : HOME_LIST_SUBSCRIBE;

   // the selection's video first, once it rests, then what is next to it on screen
   uint64_t now = sys_time_get_system_time();
   if (selected != lastSelected) { lastSelected = selected; selectedSinceUs = now; }
   const char *wanted[LIST_VISIBLE + 4];
   int wantedCount = 0;
   if (now - selectedSinceUs >= DETAILS_REST_US) {
      wanted[wantedCount++] = results->items[selected].videoId;
      for (int i = selected + 1; i <= selected + 3 && i < count; i++) wanted[wantedCount++] = results->items[i].videoId;
      if (selected > 0) wanted[wantedCount++] = results->items[selected - 1].videoId;
      for (int i = top; i < top + LIST_VISIBLE && i < count && wantedCount < LIST_VISIBLE + 4; i++)
         if (i < selected - 1 || i > selected + 3) wanted[wantedCount++] = results->items[i].videoId;
   }
   updateVideoDetails(wanted, wantedCount);
   return HOME_LIST_PASS;
}

// ---- drawing ----

static void drawRow(const VideoGrid *grid, const SearchResults *results, int index, int y, int selected)
{
   const SearchResult *item = &results->items[index];
   RowSlot *slot = &rows[index % ROW_SLOTS];
   uint32_t behind = selected ? activeTheme->surface1 : activeTheme->appBg;
   if (selected) uiFillRounded(LIST_X, y, LIST_W, ROW_H, ROW_RADIUS, fade(activeTheme->surface1));

   int tx = LIST_X + ROW_THUMB_X, ty = y + ROW_THUMB_Y;
   const GfxTexture *thumb = gridThumbnail(grid, index);
   if (thumb) drawGfxTexture(tx, ty, ROW_THUMB_W, ROW_THUMB_H, *thumb, 0.0f, 0.0f, 1.0f, 1.0f, fade(0xFFFFFFFF), GFX_FILTER_LINEAR);
   else uiFillRect(tx, ty, ROW_THUMB_W, ROW_THUMB_H, fade(activeTheme->surface));
   uiDrawImageCorners(tx, ty, ROW_THUMB_W, ROW_THUMB_H, ROW_THUMB_RADIUS, behind);
   if (selected && !focusButtons) uiDrawFocusRing(LIST_X, y, LIST_W, ROW_H, UI_RING_ROW, fade(activeTheme->focusBorder));

   int x = LIST_X + ROW_TEXT_X;
   drawAtBaseline(&slot->title, x, y + ROW_TITLE_BASE, activeTheme->textPrimary);
   int known = slot->quality[0] >= '0' && slot->quality[0] <= '9';   // a height, not "..." or a word
   uint32_t qualityColor = item->isLive ? activeTheme->live : known ? activeTheme->accent : activeTheme->textTertiary;
   drawAtBaseline(&slot->quality_, x, y + ROW_QUALITY_BASE, qualityColor);
   int metaX = x;
   if (slot->indent) {
      drawAvatar(item->channelId, x, y + ROW_META_BASE - 7 - ROW_AVATAR / 2, ROW_AVATAR, behind);
      metaX += ROW_AVATAR + ROW_AVATAR_GAP;
   }
   drawAtBaseline(&slot->meta, metaX, y + ROW_META_BASE, selected ? activeTheme->textSecondary : activeTheme->textTertiary);
}

static void drawButton(int x, int y, const Label *label, int focused)
{
   int w = label->tt.tex.w + 2 * BUTTON_PAD;
   uiFillPill(x, y, w, BUTTON_H, fade(focused ? activeTheme->textPrimary : activeTheme->surface));
   drawTint(label, x + BUTTON_PAD, uiLabelYForCenter(label, y, BUTTON_H), focused ? activeTheme->surface1 : activeTheme->textPrimary);
   if (focused) uiDrawFocusRing(x, y, w, BUTTON_H, UI_RING_ROW, fade(activeTheme->focusBorder));
}

static void drawPanel(const SearchResult *item)
{
   int x = PANEL_X;
   int base = PANEL_TITLE_BASE;
   drawAtBaseline(&panelTitle, x, base, activeTheme->textPrimary);
   base = lastBaseline(&panelTitle, base) + 46;
   drawAtBaseline(&panelMeta, x, base, activeTheme->textSecondary);

   // the chips: the quality (or that it is being looked up), the duration or LIVE, and what else the video has
   int chipY = base + 24, chipX = x;
   const VideoDetails *details = findVideoDetails(item->videoId);
   if (qualityChip.tt.tex.w > 0)
      chipX += drawChip(chipX, chipY, PANEL_CHIP_H, PANEL_CHIP_PAD, &qualityChip, activeTheme->accent, activeTheme->onAccent) + PANEL_CHIP_GAP;
   chipX += drawChip(chipX, chipY, PANEL_CHIP_H, PANEL_CHIP_PAD, &liveChip, activeTheme->live, 0xFFFFFFFF);
   if (liveChip.tt.tex.w > 0) chipX += PANEL_CHIP_GAP;
   int w = drawChip(chipX, chipY, PANEL_CHIP_H, PANEL_CHIP_PAD, &durationChip, activeTheme->surface, activeTheme->textPrimary);
   if (w) chipX += w + PANEL_CHIP_GAP;
   const Label *extras[3] = { &subsChip, &audioChip, &rewindChip };
   for (int e = 0; e < 3; e++) {
      w = drawChip(chipX, chipY, PANEL_CHIP_H, PANEL_CHIP_PAD, extras[e], activeTheme->surface, activeTheme->textSecondary);
      if (w) chipX += w + PANEL_CHIP_GAP;
   }
   if (!details) drawTint(&checkingLabel, chipX + 6, uiLabelYForCenter(&checkingLabel, chipY, PANEL_CHIP_H), activeTheme->textTertiary);

   base = chipY + PANEL_CHIP_H + 40;
   if (noteLabel.tt.tex.w > 0) {
      drawAtBaseline(&noteLabel, x, base, activeTheme->textSecondary);
      base = lastBaseline(&noteLabel, base) + 22;
   } else {
      base -= 14;
   }

   // the description: as many whole lines as the room above the channel bar holds
   if (descTex.tex.w > 0 && descFont) {
      UiTextMetrics m = uiGetTextMetrics(descFont, PANEL_DESC_SIZE);
      int room = BAR_Y - 26 - base;
      if (room >= m.texH) {
         int lines = (room - m.texH) / (m.lineH > 0 ? m.lineH : 30) + 1;
         int shown = (lines - 1) * m.lineH + m.texH;
         if (shown > descTex.tex.h) shown = descTex.tex.h;
         drawGfxTexture(x, base, descTex.tex.w, shown, descTex.tex, 0.0f, 0.0f, 1.0f, (float)shown / (float)descTex.tex.h,
                        fade(activeTheme->textSecondary), GFX_FILTER_NEAREST);
      }
   }

   // the channel's bar: its picture and name, Channel and Subscribe
   uiFillRounded(x, BAR_Y, PANEL_W, BAR_H, UI_RADIUS_PANEL, fade(activeTheme->surface1));
   int ax = x + BAR_PAD, ay = BAR_Y + (BAR_H - BAR_AVATAR) / 2;
   if (item->channelId[0]) drawAvatar(item->channelId, ax, ay, BAR_AVATAR, activeTheme->surface1);
   else uiFillCircle(ax, ay, BAR_AVATAR, fade(activeTheme->surface));
   int nameX = ax + BAR_AVATAR + 20;
   int stateShown = channelState.tt.tex.w > 0;
   int nameBase = BAR_Y + (stateShown ? 52 : 66);
   drawAtBaseline(&channelName, nameX, nameBase, activeTheme->textPrimary);
   if (stateShown) drawAtBaseline(&channelState, nameX, nameBase + 32, activeTheme->accent);
   if (item->channelId[0]) {
      int subscribeW = subscribeButton.tt.tex.w + 2 * BUTTON_PAD, channelW = channelButton.tt.tex.w + 2 * BUTTON_PAD;
      int bx = x + PANEL_W - BAR_PAD - subscribeW, by = BAR_Y + (BAR_H - BUTTON_H) / 2;
      drawButton(bx, by, &subscribeButton, focusButtons && button == 1);
      drawButton(bx - BUTTON_GAP - channelW, by, &channelButton, focusButtons && button == 0);
   }
}

void drawHomeList(VideoGrid *grid)
{
   float open = openness();
   if (open <= 0.0f) return;
   layerAlpha = (int)(open * 255.0f + 0.5f);
   if (layerAlpha > 255) layerAlpha = 255;
   int dy = (int)((1.0f - open) * (float)SLIDE_PX + 0.5f);
   uint32_t bg = activeTheme->appBg;

   // the page under it goes dark, the selection's picture comes up behind the details
   uiFillRect(0, 0, UI_SCREEN_W, UI_SCREEN_H, fade(bg));
   int ready = gridStage(grid) == GRID_READY && gridResults(grid)->count > 0;
   if (ready && drawGridHeroPicture(grid, PICTURE_X, 0, PICTURE_W, PICTURE_H, fade(0xFFFFFFFF))) {
      UiGradientStop across[3] = { { 0.0f, fade(uiWithAlpha(bg, 255)) }, { 0.32f, fade(uiWithAlpha(bg, 208)) },
                                   { 1.0f, fade(uiWithAlpha(bg, 140)) } };
      uiFillGradientH(PICTURE_X, 0, PICTURE_W, PICTURE_H, across, 3);
      UiGradientStop down[2] = { { 0.0f, fade(uiWithAlpha(bg, 0)) }, { 1.0f, fade(uiWithAlpha(bg, 255)) } };
      uiFillGradientV(0, PICTURE_FADE_TOP, UI_SCREEN_W, PICTURE_H - PICTURE_FADE_TOP + 1, down, 2);
      UiGradientStop header[2] = { { 0.0f, fade(uiWithAlpha(bg, 200)) }, { 1.0f, fade(uiWithAlpha(bg, 0)) } };
      uiFillGradientV(PICTURE_X, 0, PICTURE_W, 170, header, 2);
   }
   if (!ready) return;

   const SearchResults *results = gridResults(grid);
   int selected = gridSelectedIndex(grid);
   keepInView(selected, results->count);
   int end = top + LIST_VISIBLE < results->count ? top + LIST_VISIBLE : results->count;
   for (int i = top; i < end; i++) labelRow(results, i);
   const SearchResult *item = &results->items[selected];
   labelPanel(item);

   // the glow under the selected row goes down before any row (it reaches past its neighbours)
   int selectedY = LIST_TOP + (selected - top) * ROW_STEP + dy;
   if (selected >= top && selected < end && !focusButtons)
      uiDrawFocusGlow(LIST_X, selectedY, LIST_W, ROW_H, fade(activeTheme->focusGlow));
   for (int i = top; i < end; i++) drawRow(grid, results, i, LIST_TOP + (i - top) * ROW_STEP + dy, i == selected);
   drawPanel(item);
}

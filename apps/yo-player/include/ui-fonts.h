#pragma once

// ui-fonts - the fonts every screen shares (the Ember look, UEBERGABE 3.3): Manrope in four weights,
// opened once from the copies embedded in the app (src/assets, see ui-assets.h), and the console's own
// FONT_POP, opened once as the fallback for text Manrope cannot draw (Japanese, Korean, emoji ...).
//
// Until v0.13 every screen opened FONT_POP for itself and closed it again in its term. That cannot work
// for Manrope: a font opened from memory takes one of only 8 user-font slots (the icon font holds one
// already), so a screen stack three deep would run out. Hence one module that opens everything once.
//
// The rules for screens:
//  - Never open or close a font yourself. Take one from uiGetFont / uiGetFallbackFont and never pass it
//    to closeFont: that would blank every label on every screen that shares it.
//  - Fixed UI text (English, written in the code) goes through uiInitLabel: always Manrope.
//  - Text from outside - video titles, channel names, search queries, playlist names, chapter titles,
//    track names - goes through uiInitTextLabel / uiSetTextLabel: drawn raw (a '{' in a title is a
//    character, not markup) and in Manrope only when Manrope has every character of it; otherwise the
//    WHOLE text is drawn in FONT_POP, as all text was before.
//  - Subtitles stay FONT_POP (uiGetFallbackFont).
//  - Place text with uiPlaceLabelBaseline / uiPlaceLabelCentered. Manrope's and FONT_POP's textures put
//    the glyphs at different heights, so a position worked out from the font size is right for at most
//    one of them.

#include "font.h"        // Font, TextWrap
#include "ui/label.h"    // Label
#include <stdint.h>

typedef enum {
   UI_WEIGHT_REGULAR,     // 400
   UI_WEIGHT_SEMIBOLD,    // 600
   UI_WEIGHT_BOLD,        // 700
   UI_WEIGHT_EXTRABOLD,   // 800
   UI_WEIGHT_COUNT
} UiWeight;

// ---- the Ember type scale (UEBERGABE 3.3): size in px on the 1920x1080 canvas + the weight with it ----
#define UI_DISPLAY_SIZE        66                    // hero title on the start screen (Phase 4)
#define UI_DISPLAY_WEIGHT      UI_WEIGHT_EXTRABOLD
#define UI_PAGE_TITLE_SIZE     46                    // "Settings", "Subscriptions" (the design allows 46-52)
#define UI_PAGE_TITLE_WEIGHT   UI_WEIGHT_EXTRABOLD
#define UI_SHELF_TITLE_SIZE    32                    // shelf and panel titles (30-34)
#define UI_SHELF_TITLE_WEIGHT  UI_WEIGHT_EXTRABOLD
#define UI_CARD_TITLE_SIZE     22                    // video titles in cards and rows (22-24)
#define UI_CARD_TITLE_WEIGHT   UI_WEIGHT_BOLD
#define UI_META_SIZE           20                    // channel - views - date
#define UI_META_WEIGHT         UI_WEIGHT_SEMIBOLD
#define UI_BADGE_SIZE          18                    // LIVE, 24:18, L1, START - the smallest text there is
#define UI_BADGE_WEIGHT        UI_WEIGHT_EXTRABOLD
#define UI_BODY_SIZE           22                    // descriptions, explanations, notices (22-24)
#define UI_BODY_WEIGHT         UI_WEIGHT_REGULAR
#define UI_HINT_SIZE           20                    // button-hint captions (ui-hints.h draws them)
#define UI_HINT_WEIGHT         UI_WEIGHT_SEMIBOLD

// Legibility floor at 1920x1080 (UEBERGABE 2.4): reading text never below UI_MIN_TEXT_SIZE, badges and key
// captions never below UI_MIN_BADGE_SIZE and then in weight 800. textTertiary only at UI_MIN_TEXT_SIZE or
// larger - 18 px grey text has already proved unreadable on the TV once.
#define UI_MIN_TEXT_SIZE       20
#define UI_MIN_BADGE_SIZE      18

// ---- lifetime ----

// Opens FONT_POP and the four Manrope weights. Call once after initFont() (main.c does), before anything
// creates a label. Never fails: a weight that does not open is replaced by the nearest one that did, and
// if none did, every getter returns FONT_POP.
void initUiFonts(void);

// Closes them again. main.c only, at exit - after the last screen and the download overlay are gone and
// before termFont().
void termUiFonts(void);

// ---- the fonts ----

// 1 when at least one Manrope weight is open (0 means the whole app is drawing in FONT_POP).
int   uiHasManrope(void);

// Manrope in that weight - or the nearest weight that opened, or FONT_POP when none did. Never NULL; the
// pointer stays valid for the life of the app. For widgets that still take a Font* (VideoGrid, Sidebar,
// VolumeMeter ...) pass uiGetFont(...) there. Use it directly only for FIXED text: external text must go
// through uiPickFont (or the label helpers below), or a Japanese title renders as blanks.
Font *uiGetFont(UiWeight weight);

// FONT_POP, shared. Subtitles, and anything else that must be the console's font. Never NULL.
Font *uiGetFallbackFont(void);

// 1 when Manrope can draw every character of text: each UTF-8 sequence is decoded exactly the way the
// library's renderer decodes it and looked up in manrope-coverage.h. '\n', '\r' and '\t' are layout, not
// glyphs, and are skipped. 0 on the first character Manrope lacks, and on malformed UTF-8. NULL or "" -> 1.
int   uiTextFitsManrope(const char *text);

// the font to render this EXTERNAL text in: uiGetFont(weight) when uiTextFitsManrope(text), else FONT_POP
// - for the whole text, never mixed. Cheap (no rendering); still, call it when the text changes, not per
// frame.
Font *uiPickFont(UiWeight weight, const char *text);

// ---- labels ----
// All of these take the same position/width/size/colour/wrap arguments as the library's initLabel (minus
// its unused height). width is the wrap/clip width in px, or AUTO (0) for a single line up to 1024 px.
// The colour is baked into the texture: pass an opaque theme colour.

// A label for FIXED UI text, in Manrope (markup such as {b} is parsed, as with initLabel). Change its text
// later with the library's setLabelText; its font stays.
void uiInitLabel(Label *l, UiWeight weight, int x, int y, int width, int size, uint32_t color, TextWrap wrap,
                 const char *text);

// A label for EXTERNAL text: raw (no markup), font picked with uiPickFont. text may be NULL or "".
void uiInitTextLabel(Label *l, UiWeight weight, int x, int y, int width, int size, uint32_t color,
                     TextWrap wrap, const char *text);

// Sets EXTERNAL text on a label made by uiInitTextLabel (or any label - it becomes raw). Picks the font
// again and, when that changes, drops the old texture and renders anew: a Label binds its font when it
// renders and setLabelText skips text it already shows, so a plain setLabelText would keep a Japanese
// title in Manrope (blank) or a Latin one in FONT_POP. Unchanged text in an unchanged font renders
// nothing (a copy, the coverage check and a compare). text may be NULL (= ""), and may be the label's own
// l->text. Text longer than a Label holds (255 bytes) is cut at a character boundary, never inside a UTF-8
// sequence.
void uiSetTextLabel(Label *l, UiWeight weight, const char *text);

// Re-renders a label's current text in another weight, size or colour (e.g. a row that turns 800 when
// focused). A raw label (external text) gets its font picked again, any other label stays in Manrope.
// A no-op when nothing changes; otherwise one rasterisation, so call it when the state changes.
void uiSetLabelStyle(Label *l, UiWeight weight, int size, uint32_t color);

// ---- placing text ----
// A label texture does not start at the top of its line box: the renderer crops it a few rows into the
// first line, by an amount that differs between Manrope and FONT_POP. These helpers measure where the
// capitals and the baseline really land, once per (font, size) by rendering an "H" (cached, 64 entries),
// and place by that. All values are texture rows, i.e. relative to the label's y.

typedef struct {
   int texH;       // height of a one-line texture
   int capTop;     // first row with ink of a capital ("H")
   int baseline;   // the row just below a capital's ink = the baseline
   int lineH;      // the pitch between wrapped lines
} UiTextMetrics;

// The metrics for text of `size` px in `font` (any Font*, also one a screen opened itself). Measuring
// needs gfx, so the first call for a (font, size) must come after initGfx; until a measurement succeeds
// a size-based estimate is returned (and not cached).
UiTextMetrics uiGetTextMetrics(Font *font, int size);

// y for a texture of (font, size) so that its baseline lands on baselineY (CSS baseline -> PS3 y).
int  uiTextYForBaseline(Font *font, int size, int baselineY);
// y for a one-line texture of (font, size) so that its capitals sit centred in [boxY, boxY + boxH): the
// right way to centre text in a badge, chip, button or row. Works for digits too (they are cap height).
int  uiTextYForCenter(Font *font, int size, int boxY, int boxH);

// the same for a label (its own font and size)
int  uiLabelYForBaseline(const Label *l, int baselineY);
int  uiLabelYForCenter(const Label *l, int boxY, int boxH);

// moveLabel to x and the y from uiLabelYForBaseline / uiLabelYForCenter
void uiPlaceLabelBaseline(Label *l, int x, int baselineY);
void uiPlaceLabelCentered(Label *l, int x, int boxY, int boxH);

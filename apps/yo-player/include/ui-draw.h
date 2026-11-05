#pragma once

// ui-draw - the drawing pieces of the Ember look (UEBERGABE 3.4 and 4): gradients, rounded fills, rounded
// image corners, the focus ring and its glow, badges and chips, progress bars, smooth circles, outlined
// text and fixed-width numbers. The library draws only flat rectangles, so every rounded or soft shape here
// is one of eleven small white textures embedded in the app (src/assets/ember-textures.c, see ui-assets.h),
// stretched as a 9-slice and tinted to the colour asked for.
//
// Rules for screens:
//  - Colours come from activeTheme (theme.h), never as literals; uiWithAlpha derives a translucent stop
//    from a theme colour. Plain white (0xFFFFFFFF) as a "do not tint" multiplier is fine.
//  - Everything draws immediately, in call order: later calls paint over earlier ones. The focus glow goes
//    down BEFORE the card it surrounds, the ring after it.
//  - A helper texture that failed to load degrades to a flat shape (a rectangle, a square ring) or to
//    nothing (glow, image corners) - never a crash. The log says which one failed.
//  - Coordinates are the 1920x1080 canvas, like every other draw call.

#include "gfx.h"         // GfxTexture
#include "font.h"        // Font
#include "ui/label.h"    // Label
#include "ui-fonts.h"    // UiWeight
#include <stdint.h>

// ---- layout tokens (UEBERGABE 2.5 and 3.4) ----
#define UI_SCREEN_W       1920
#define UI_SCREEN_H       1080
#define UI_SAFE_X         96                           // content keeps 96 px from the left and right edges
#define UI_SAFE_RIGHT     (UI_SCREEN_W - UI_SAFE_X)    // 1824: nothing to the right of this (the rail excepted)
#define UI_SAFE_TOP       48                           // and 48 px (up to 54) from the top and the bottom
#define UI_SAFE_BOTTOM    (UI_SCREEN_H - UI_SAFE_TOP)  // 1032: nothing below this line

#define UI_RADIUS_BADGE   8     // badges, key chips
#define UI_RADIUS_THUMB   14    // thumbnails in the grid and in search
#define UI_RADIUS_CARD    16    // cards on the Cinema shelf
#define UI_RADIUS_ROW     18    // rows, segment groups, the skip panel
#define UI_RADIUS_PANEL   28    // panels

// which edge an x coordinate names, for things laid out along a line (the hint row, ui-hints.h)
typedef enum {
   UI_ALIGN_LEFT,     // x is the left edge
   UI_ALIGN_CENTER,   // x is the middle
   UI_ALIGN_RIGHT     // x is the right edge
} UiAlign;

// the same colour with another alpha (0..255) - for gradient stops and translucent fills derived from a
// theme colour, e.g. uiWithAlpha(activeTheme->gradientBase, 0xC7) for the player's 78 % top gradient
static inline uint32_t uiWithAlpha(uint32_t argb, int alpha)
{
   if (alpha < 0) alpha = 0;
   if (alpha > 255) alpha = 255;
   return ((uint32_t)alpha << 24) | (argb & 0x00FFFFFFu);
}

// ---- lifetime ----

// Loads the eleven helper textures. Call once, AFTER initGfx (an upload before it breaks the VRAM
// allocator and initGfx itself would fail) - main.c does, after loadConsoleGlyphs. Logs any texture that
// did not load and whether PNG alpha survived the decoder. Returns how many loaded (11 = all).
int  initUiDraw(void);

// finishGfx, then frees the helper textures and every circle uiFillCircle generated. main.c, at exit.
void termUiDraw(void);

// Logs VRAM use as "[vram] <where>: used N KB, free N KB, largest free block N KB" - from
// getUsedVram / getFreeVram / getLargestFreeBlock. Call it where VRAM should come back to the same level
// (page turns, leaving a screen) so dbg.txt can show it stays stable. Not per frame.
void uiLogVram(const char *where);

// ---- the helper textures, for a shape none of the functions below covers ----

typedef enum {
   UI_TEX_ROUND_R8,     // 32x32 filled, radius 8            caps 8
   UI_TEX_ROUND_R16,    // 48x48 filled, radius 16           caps 16
   UI_TEX_ROUND_R28,    // 64x64 filled, radius 28           caps 28
   UI_TEX_CORNER_R14,   // 14x14 outer corner mask, top left  (not for 9-slicing)
   UI_TEX_CORNER_R16,   // 16x16 outer corner mask, top left  (not for 9-slicing)
   UI_TEX_RING_R20_W3,  // 48x48 ring 3 px, outer radius 20  caps 20
   UI_TEX_RING_R23_W4,  // 64x64 ring 4 px, outer radius 23  caps 24
   UI_TEX_RING_R25_W4,  // 64x64 ring 4 px, outer radius 25  caps 24
   UI_TEX_RING_R34_W4,  // 80x80 ring 4 px, outer radius 34  caps 36
   UI_TEX_GLOW_R23,     // 192x192 soft glow around a radius-23 rectangle, peak alpha 95/255   caps 95
   UI_TEX_CIRCLE_64,    // 64x64 filled circle                caps 32 (a 9-slice of it is a pill)
   UI_TEX_COUNT
} UiTexture;

// the texture (a zeroed one, .offset == 0, when it did not load) and its 9-slice cap in source px
GfxTexture uiGetTexture(UiTexture id);
int        uiGetTextureCaps(UiTexture id);

// draws texture `id` as a 9-slice over (x, y, w, h), tinted: corners `caps * scale` px, edges and middle
// stretched. scale 1 = the texture's own corner size; keep it <= 1 and, where you can, >= 0.6 (there are
// no mipmaps, so a smaller corner aliases). Draws nothing when the texture is missing or w/h <= 0.
void uiDrawNineSlice(UiTexture id, int x, int y, int w, int h, float scale, uint32_t tint);

// ---- fills ----

// a plain rectangle (fillGfxRectangle; clamped to the screen)
void uiFillRect(int x, int y, int w, int h, uint32_t argb);

// one stop of a gradient: at = 0.0 (top/left edge) .. 1.0 (bottom/right edge), like a CSS percentage
typedef struct {
   float    at;
   uint32_t argb;
} UiGradientStop;

// A vertical gradient over (x, y, w, h) through `count` stops (sorted by `at`, 2 or more): one band of two
// triangles between each pair of stops, and - as in CSS - the first stop's colour above the first stop and
// the last one's below the last. A fully transparent stop takes the RGB of its neighbour, so a fade to
// "transparent" never passes through grey. Example, the player's bottom fade:
//    UiGradientStop s[3] = { {0.0f, uiWithAlpha(base, 0)}, {0.45f, uiWithAlpha(base, 0xB3)},
//                            {1.0f, uiWithAlpha(base, 0xF0)} };   // base = activeTheme->gradientBase
//    uiFillGradientV(0, 620, 1920, 460, s, 3);
void uiFillGradientV(int x, int y, int w, int h, const UiGradientStop *stops, int count);
// the same, left to right
void uiFillGradientH(int x, int y, int w, int h, const UiGradientStop *stops, int count);
// two-stop shortcut: top colour -> bottom colour
void uiFillGradient2V(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

// A rectangle with rounded corners of `radius` px (clamped to half the shorter side; 0 = square). Picks the
// smallest helper texture whose radius is not below the one asked for, so the corners are only ever
// scaled down: up to 8 rund-r8, up to 16 rund-r16, above that rund-r28 (radius 18 = rund-r28 at 0.643).
void uiFillRounded(int x, int y, int w, int h, int radius, uint32_t argb);

// a pill: uiFillRounded with radius = half the shorter side
void uiFillPill(int x, int y, int w, int h, uint32_t argb);

// ---- images ----

// Rounds an image that was just drawn at (x, y, w, h) by laying the corner masks (ecke-r14 for radius
// <= 14, else ecke-r16, drawn at `radius` px) over its four corners, mirrored through the UVs. behindArgb
// is the colour BEHIND the image - appBg on a page, surface1 under a focused search row - because the
// masks paint that colour over the bits outside the rounding. Draw progress bars on the image first, so
// the masks round them too.
void uiDrawImageCorners(int x, int y, int w, int h, int radius, uint32_t behindArgb);

// ---- focus ----

typedef enum {
   UI_RING_THUMB,    // around an image with radius 14 (grid, search): ring-r23-w4, 9 px outside the box
   UI_RING_CARD,     // around a card with radius 16 (Cinema shelf):   ring-r25-w4, 9 px outside the box
   UI_RING_ROW,      // around a search row with radius 26:            ring-r34-w4, 8 px outside the box
   UI_RING_SEGMENT   // around a settings segment with radius 14:      ring-r20-w3, 6 px outside the box
} UiRing;

// The soft glow under a focused card or row, (x, y, w, h) being the card itself: schein-r23 stretched to
// the card + 81 px on every side, tinted (activeTheme->focusGlow). Draw it FIRST, before the card - it is
// meant to show around the card, not over it.
void uiDrawFocusGlow(int x, int y, int w, int h, uint32_t tint);

// the same glow with its spread scaled (target = shape + 81*scale px; scale 1 = uiDrawFocusGlow), e.g. the
// player knob's smaller halo with activeTheme->knobGlow
void uiDrawGlow(int x, int y, int w, int h, float scale, uint32_t tint);

// The focus ring around the box (x, y, w, h) - the card, row or segment itself, not the ring's outer
// edge: the helper adds the gap. tint: activeTheme->focusBorder, or textPrimary for a selected segment.
// Without its texture it falls back to a square ring of the same width.
void uiDrawFocusRing(int x, int y, int w, int h, UiRing ring, uint32_t tint);

// ---- text on shapes ----
// The labels below are drawn where these functions put them, at their own baked colour (as drawLabel
// does); the label's x/y is NOT changed, so one label can be drawn in several places.

// draws a label at (x, y) without moving it - drawLabel at another position
void uiDrawLabelAt(const Label *label, int x, int y);

#define UI_BADGE_H        30    // Ember badge: 30 px tall, 9 px either side of the text, radius 8, text 18/800
#define UI_BADGE_PAD_X    9

// A rounded box `h` px tall around a one-line label: width = label width + 2 * padX, the label padX in from
// the left and its capitals centred vertically (uiLabelYForCenter). Returns the width it drew.
int  uiDrawTextBox(int x, int y, int h, int padX, int radius, const Label *label, uint32_t fillArgb);
// the width uiDrawTextBox would draw (for right-aligning: x = right - uiGetTextBoxWidth(...))
int  uiGetTextBoxWidth(const Label *label, int padX);

// an Ember badge (duration, LIVE, WATCHED, Watch Later): uiDrawTextBox(x, y, UI_BADGE_H, UI_BADGE_PAD_X,
// UI_RADIUS_BADGE, ...). Make the label UI_BADGE_SIZE (18) in UI_BADGE_WEIGHT (800 - 18 px text is only
// legible on the TV that heavy). Returns the width.
int  uiDrawBadge(int x, int y, const Label *label, uint32_t fillArgb);
static inline int uiGetBadgeWidth(const Label *label) { return uiGetTextBoxWidth(label, UI_BADGE_PAD_X); }

// a pill-shaped chip h px tall (radius h/2). Returns the width.
int  uiDrawChip(int x, int y, int h, int padX, const Label *label, uint32_t fillArgb);

// ---- bars and dots ----

// A progress bar: the whole track in trackArgb, the first `fraction` (clamped 0..1) of it in fillArgb on
// top. radius 0 = square ends (a bar inside a thumbnail, rounded afterwards by uiDrawImageCorners), else
// both pieces are rounded with that radius (h/2 = pill ends).
void uiDrawProgressBar(int x, int y, int w, int h, float fraction, uint32_t trackArgb, uint32_t fillArgb,
                       int radius);

// A smooth filled circle of `diameter` px with its top-left at (x, y). Below 32 px the circle is rendered
// at exactly that size the first time it is needed (anti-aliased, cached per diameter) - kreis-64 scaled
// that far down would stair-step, as there are no mipmaps; from 32 px up it is kreis-64.
void uiFillCircle(int x, int y, int diameter, uint32_t argb);
// the same, centred on (cx, cy)
static inline void uiFillCircleAt(int cx, int cy, int diameter, uint32_t argb)
{
   uiFillCircle(cx - diameter / 2, cy - diameter / 2, diameter, argb);
}

// ---- outlined text ----

// Draws a label with a dark outline, for text over the video: the label's texture 8 times, offset by
// `radius` px in each direction and tinted outlineArgb, then the label itself on top. The tint multiplies
// the label's baked colour, so a near-black outline (activeTheme->gradientBase) is near-black whatever
// the text colour. radius 2 suits 1080p (it is ~1.3 real px at 720p). Costs 9 quads, no extra texture.
void uiDrawLabelOutlined(const Label *label, uint32_t outlineArgb, int radius);

// ---- fixed-width numbers ----
// Manrope's digits are proportional ('1' is two thirds as wide as '0') and the renderer has no tabular
// figures, so a counting time or volume figure would wobble as it changes. UiDigits pre-renders the
// characters below once, in white, and draws a string of them with every digit stepping by the widest
// digit's advance (each digit centred in that cell) and tinted when drawn. Other characters advance by
// their own width; characters outside the set are skipped.
#define UI_DIGIT_CHARS  "0123456789:/%. "
#define UI_DIGIT_COUNT  15

typedef struct {
   Label glyphs[UI_DIGIT_COUNT];    // one white label per UI_DIGIT_CHARS character (the space has none)
   int   advance[UI_DIGIT_COUNT];   // px each character steps the pen
   int   digitCell;                 // the widest digit's advance: what every digit steps by
   Font *font;
   int   size;
} UiDigits;

// renders the set in Manrope (FONT_POP if Manrope is missing) at `size` px in `weight`. Needs gfx. A few
// small textures; free with uiFreeDigits in the screen's term.
void uiInitDigits(UiDigits *digits, UiWeight weight, int size);
void uiFreeDigits(UiDigits *digits);

// the width uiDrawDigits will take for text
int  uiGetDigitsWidth(const UiDigits *digits, const char *text);

// Draws text with its texture top at y (the same anatomy as a label of digits->font / digits->size, so
// uiTextYForBaseline(digits->font, digits->size, ...) and uiTextYForCenter give the y) and its left edge at
// x, tinted `color` (opaque theme colour). Right-align with x = right - uiGetDigitsWidth(...).
void uiDrawDigits(const UiDigits *digits, int x, int y, const char *text, uint32_t color);
// the same with a dark outline (see uiDrawLabelOutlined): every outline first, then every digit
void uiDrawDigitsOutlined(const UiDigits *digits, int x, int y, const char *text, uint32_t color,
                          uint32_t outlineArgb, int radius);

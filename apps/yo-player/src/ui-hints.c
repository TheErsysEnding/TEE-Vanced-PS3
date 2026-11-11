// ui-hints - the Ember button-hint row (see ui-hints.h).

#include "ui-hints.h"
#include "ui-fonts.h"   // uiInitLabel, UI_HINT_SIZE / UI_HINT_WEIGHT, uiLabelYForCenter
#include "theme.h"      // activeTheme->textSecondary
#include "dbg.h"        // logInfo
#include "gfx.h"        // uploadGfxTexture, drawGfxTexture
#include <string.h>     // memset
#include <stdio.h>      // snprintf
#include <stdlib.h>     // malloc
#include <math.h>       // sqrtf

// Once per run: the console glyphs' own pixel sizes. The art is decoded from the console's imagefont.bin
// and nothing in code says how big it is, so this line answers whether UI_HINT_GLYPH_H scales it up or down.
static void logGlyphSizesOnce(void)
{
   static int logged;
   if (logged) return;
   logged = 1;

   char sizes[256];
   int length = 0;
   sizes[0] = '\0';
   for (int glyph = 0; glyph < GLYPH_COUNT && length < (int)sizeof sizes - 12; glyph++) {
      GfxTexture tex = getConsoleGlyph((ConsoleGlyph)glyph);
      length += snprintf(sizes + length, sizeof sizes - (size_t)length, "%s%dx%d", glyph ? " " : "", tex.w, tex.h);
   }
   logInfo("[ui] console glyphs (X O square triangle L1 R1 L2 R2 SELECT START L3 R3 up down left right): %s\n", sizes);
}

// ---- the buttons (V3.0.0) ----
// The symbols are drawn here, in white with their shape in the alpha channel, once per size they are asked for
// (no mipmaps on this console: a symbol scaled down from a bigger texture flickers at its thin strokes), and
// tinted textPrimary when drawn. Shapes in a unit square: strokes 0.17 of it wide, as the mockups' 3 px at 19.

enum { SYM_CROSS, SYM_CIRCLE, SYM_SQUARE, SYM_TRIANGLE, SYM_UP, SYM_DOWN, SYM_LEFT, SYM_RIGHT, SYM_COUNT };
#define SYM_CACHE   24
#define CAP_CACHE   24
#define STROKE_HALF 0.085f

typedef struct { int symbol, size; GfxTexture tex; } SymbolTex;
typedef struct { int glyph, height; Label label; } CapLabel;
static SymbolTex symbolCache[SYM_CACHE];
static int       symbolCount;
static CapLabel  capCache[CAP_CACHE];
static int       capCount;

static float segmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
   float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
   float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
   if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
   float dx = wx - t * vx, dy = wy - t * vy;
   return sqrtf(dx * dx + dy * dy);
}

// signed distance to a triangle's edges: negative inside
static float triangleDistance(float px, float py, const float *v)
{
   float d = 1e9f;
   for (int i = 0; i < 3; i++) {
      float e = segmentDistance(px, py, v[2 * i], v[2 * i + 1], v[2 * ((i + 1) % 3)], v[2 * ((i + 1) % 3) + 1]);
      if (e < d) d = e;
   }
   // inside when on the same side of all three edges
   int sign = 0, inside = 1;
   for (int i = 0; i < 3 && inside; i++) {
      float ax = v[2 * i], ay = v[2 * i + 1], bx = v[2 * ((i + 1) % 3)], by = v[2 * ((i + 1) % 3) + 1];
      float c = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
      int s = c > 0 ? 1 : -1;
      if (!sign) sign = s; else if (s != sign) inside = 0;
   }
   return inside ? -d : d;
}

// how much of the pixel centred at (px, py) - unit square coordinates - the symbol covers, 0..1
static float symbolCoverage(int symbol, float px, float py, float pixel)
{
   static const float tri[6]   = { 0.50f, 0.15f, 0.86f, 0.80f, 0.14f, 0.80f };
   static const float up[6]    = { 0.50f, 0.22f, 0.84f, 0.74f, 0.16f, 0.74f };
   static const float down[6]  = { 0.16f, 0.26f, 0.84f, 0.26f, 0.50f, 0.78f };
   static const float left[6]  = { 0.22f, 0.50f, 0.74f, 0.16f, 0.74f, 0.84f };
   static const float right[6] = { 0.26f, 0.16f, 0.78f, 0.50f, 0.26f, 0.84f };
   float d;   // distance from the stroke's middle (strokes) or signed distance (filled), in unit-square units
   switch (symbol) {
   case SYM_CROSS: {
      float a = segmentDistance(px, py, 0.20f, 0.20f, 0.80f, 0.80f), b = segmentDistance(px, py, 0.20f, 0.80f, 0.80f, 0.20f);
      d = (a < b ? a : b) - STROKE_HALF;
      break;
   }
   case SYM_CIRCLE: {
      float dx = px - 0.5f, dy = py - 0.5f;
      d = fabsf(sqrtf(dx * dx + dy * dy) - 0.33f) - STROKE_HALF;
      break;
   }
   case SYM_SQUARE: {
      float dx = fabsf(px - 0.5f) - 0.30f, dy = fabsf(py - 0.5f) - 0.30f;
      float outside = sqrtf((dx > 0 ? dx * dx : 0) + (dy > 0 ? dy * dy : 0));
      float box = outside + (dx > dy ? (dx < 0 ? dx : 0) : (dy < 0 ? dy : 0));
      d = fabsf(box) - STROKE_HALF;
      break;
   }
   case SYM_TRIANGLE: d = fabsf(triangleDistance(px, py, tri)) - STROKE_HALF; break;
   case SYM_UP:       d = triangleDistance(px, py, up);    break;
   case SYM_DOWN:     d = triangleDistance(px, py, down);  break;
   case SYM_LEFT:     d = triangleDistance(px, py, left);  break;
   default:           d = triangleDistance(px, py, right); break;
   }
   float a = 0.5f - d / pixel;
   return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

static GfxTexture symbolTexture(int symbol, int size)
{
   for (int i = 0; i < symbolCount; i++)
      if (symbolCache[i].symbol == symbol && symbolCache[i].size == size) return symbolCache[i].tex;
   GfxTexture tex;
   memset(&tex, 0, sizeof tex);
   if (size < 4 || size > 128 || symbolCount >= SYM_CACHE) return tex;
   uint32_t *pixels = (uint32_t *)malloc((size_t)size * size * 4);
   if (!pixels) return tex;
   float pixel = 1.0f / (float)size;
   for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++) {
         float a = symbolCoverage(symbol, ((float)x + 0.5f) * pixel, ((float)y + 0.5f) * pixel, pixel);
         pixels[y * size + x] = ((uint32_t)(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;   // ARGB, as uploadGfxTexture takes
      }
   uint32_t offset = uploadGfxTexture(pixels, size, size, size * 4);
   free(pixels);
   if (!offset) return tex;
   tex.offset = offset;
   tex.w = tex.h = size;
   tex.pitch = (size * 4 + 63) & ~63;
   symbolCache[symbolCount].symbol = symbol;
   symbolCache[symbolCount].size = size;
   symbolCache[symbolCount].tex = tex;
   symbolCount++;
   return tex;
}

static int glyphSymbol(ConsoleGlyph glyph)
{
   switch (glyph) {
   case GLYPH_CROSS:      return SYM_CROSS;
   case GLYPH_CIRCLE:     return SYM_CIRCLE;
   case GLYPH_SQUARE:     return SYM_SQUARE;
   case GLYPH_TRIANGLE:   return SYM_TRIANGLE;
   case GLYPH_DPAD_UP:    return SYM_UP;
   case GLYPH_DPAD_DOWN:  return SYM_DOWN;
   case GLYPH_DPAD_LEFT:  return SYM_LEFT;
   case GLYPH_DPAD_RIGHT: return SYM_RIGHT;
   default:               return -1;   // a key cap
   }
}

static const char *capName(ConsoleGlyph glyph)
{
   switch (glyph) {
   case GLYPH_L1: return "L1";   case GLYPH_R1: return "R1";
   case GLYPH_L2: return "L2";   case GLYPH_R2: return "R2";
   case GLYPH_L3: return "L3";   case GLYPH_R3: return "R3";
   case GLYPH_SELECT: return "SELECT";
   default:       return "START";
   }
}

// the key cap's name, rasterised once per (button, height): 800 at half the cap's height
static const Label *capLabel(ConsoleGlyph glyph, int height)
{
   for (int i = 0; i < capCount; i++)
      if (capCache[i].glyph == (int)glyph && capCache[i].height == height) return &capCache[i].label;
   if (capCount >= CAP_CACHE) return NULL;
   CapLabel *cap = &capCache[capCount++];
   cap->glyph = (int)glyph;
   cap->height = height;
   uiInitLabel(&cap->label, UI_WEIGHT_EXTRABOLD, 0, 0, AUTO, (height + 1) / 2, activeTheme->textPrimary, TEXT_NOWRAP,
               capName(glyph));
   return &cap->label;
}

static int capPad(int height) { return (height * 3 + 5) / 10; }

int uiGetButtonGlyphWidth(ConsoleGlyph glyph, int height)
{
   if (glyphSymbol(glyph) >= 0) return height;
   const Label *label = capLabel(glyph, height);
   int width = (label ? label->tt.tex.w : height) + 2 * capPad(height);
   return width < height ? height : width;
}

int uiDrawButtonGlyph(ConsoleGlyph glyph, int x, int y, int height, uint32_t fill)
{
   int symbol = glyphSymbol(glyph);
   if (symbol >= 0) {
      uiFillCircle(x, y, height, fill);
      int size = (height * 56 + 50) / 100;
      GfxTexture tex = symbolTexture(symbol, size);
      if (tex.offset)
         drawGfxTexture(x + (height - size) / 2, y + (height - size) / 2, size, size, tex, 0.0f, 0.0f, 1.0f, 1.0f,
                        activeTheme->textPrimary, GFX_FILTER_LINEAR);
      return height;
   }
   int width = uiGetButtonGlyphWidth(glyph, height);
   uiFillRounded(x, y, width, height, UI_RADIUS_BADGE, fill);
   const Label *label = capLabel(glyph, height);
   if (label) uiDrawLabelAt(label, x + (width - label->tt.tex.w) / 2, uiLabelYForCenter(label, y, height));
   return width;
}

void uiSetHintsFill(UiHints *row, uint32_t fill) { row->fill = fill; }

void uiInitHintsStyled(UiHints *row, int glyphHeight, int captionSize, uint32_t captionColor)
{
   memset(row, 0, sizeof *row);
   row->fill         = activeTheme->hintFill;
   row->glyphHeight  = glyphHeight > 0 ? glyphHeight : UI_HINT_GLYPH_H;
   row->captionSize  = captionSize > 0 ? captionSize : UI_HINT_SIZE;
   row->captionColor = captionColor;
   row->gap          = UI_HINT_GAP;
   logGlyphSizesOnce();
}

void uiInitHints(UiHints *row)
{
   uiInitHintsStyled(row, UI_HINT_GLYPH_H, UI_HINT_SIZE, activeTheme->textSecondary);
}

int uiAddHint(UiHints *row, ConsoleGlyph glyph, const char *caption)
{
   if (row->count >= UI_HINTS_MAX) return -1;
   UiHint *hint = &row->hints[row->count];
   hint->glyph  = glyph;
   hint->paired = (caption == NULL || caption[0] == '\0');
   hint->hidden = 0;
   // a styled row with captions under 20 px gets them in 800: lighter than that, 18-px text does not
   // survive the TV (UEBERGABE 2.4)
   UiWeight weight = row->captionSize < UI_MIN_TEXT_SIZE ? UI_WEIGHT_EXTRABOLD : UI_HINT_WEIGHT;
   uiInitLabel(&hint->caption, weight, 0, 0, AUTO, row->captionSize, row->captionColor, TEXT_NOWRAP,
               caption ? caption : "");
   return row->count++;
}

void uiSetHintCaption(UiHints *row, int index, const char *caption)
{
   if (index < 0 || index >= row->count) return;
   UiHint *hint = &row->hints[index];
   setLabelText(&hint->caption, caption ? caption : "");   // the library's setLabelText would crash on NULL
   hint->paired = (caption == NULL || caption[0] == '\0');
}

void uiSetHintShown(UiHints *row, int index, int shown)
{
   if (index < 0 || index >= row->count) return;
   row->hints[index].hidden = !shown;
}

void uiSetHintsGap(UiHints *row, int gap)
{
   row->gap = gap > 0 ? gap : UI_HINT_GAP;
}

// the button at the row's height (a round chip, or a key cap as wide as its name)
static int glyphWidth(const UiHints *row, ConsoleGlyph glyph)
{
   return uiGetButtonGlyphWidth(glyph, row->glyphHeight);
}

static int hintWidth(const UiHints *row, const UiHint *hint)
{
   int width = glyphWidth(row, hint->glyph);
   if (!hint->paired) width += UI_HINT_CAPTION_GAP + hint->caption.tt.tex.w;
   return width;
}

// what follows a hint: the tight pair gap after a caption-less glyph, the row's gap after anything else.
// Taken from the previous VISIBLE hint, so a hidden one in between leaves no hole and no double gap.
static int gapAfter(const UiHints *row, const UiHint *hint)
{
   return hint->paired ? UI_HINT_PAIR_GAP : row->gap;
}

int uiGetHintsWidth(const UiHints *row)
{
   int total = 0;
   const UiHint *previous = NULL;
   for (int i = 0; i < row->count; i++) {
      const UiHint *hint = &row->hints[i];
      if (hint->hidden) continue;
      if (previous) total += gapAfter(row, previous);
      total += hintWidth(row, hint);
      previous = hint;
   }
   return total;
}

void uiDrawHints(const UiHints *row, int x, int y, UiAlign align)
{
   if (align == UI_ALIGN_RIGHT)       x -= uiGetHintsWidth(row);
   else if (align == UI_ALIGN_CENTER) x -= uiGetHintsWidth(row) / 2;

   const UiHint *previous = NULL;
   for (int i = 0; i < row->count; i++) {
      const UiHint *hint = &row->hints[i];
      if (hint->hidden) continue;
      if (previous) x += gapAfter(row, previous);

      x += uiDrawButtonGlyph(hint->glyph, x, y, row->glyphHeight, row->fill);

      if (!hint->paired) {
         x += UI_HINT_CAPTION_GAP;
         // centred by the capitals, not by the texture: Manrope's texture carries its descender room
         // below the baseline, so centring the texture would sit the caption visibly high
         uiDrawLabelAt(&hint->caption, x, uiLabelYForCenter(&hint->caption, y, row->glyphHeight));
         x += hint->caption.tt.tex.w;
      }
      previous = hint;
   }
}

void uiTermHints(UiHints *row)
{
   for (int i = 0; i < row->count; i++) freeLabel(&row->hints[i].caption);
   row->count = 0;
}

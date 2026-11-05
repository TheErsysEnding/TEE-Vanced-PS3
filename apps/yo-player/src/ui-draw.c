// ui-draw - the Ember drawing pieces over the library's flat primitives (see ui-draw.h).

#include "ui-draw.h"
#include "ui-assets.h"    // the embedded helper PNGs
#include "ui/slice.h"     // NineSlice
#include "dbg.h"          // logInfo / logWarn / logError
#include <cell/gcm.h>     // cellGcmGetConfiguration: reading one texel back for the alpha check
#include <string.h>       // memset / strchr
#include <math.h>         // ceilf

typedef struct {
   const unsigned char *data;
   const unsigned int  *size;
   int                  caps;   // 9-slice cap in source px: the radius the shape keeps at its corners
   const char          *name;   // the handover's file name, for the log
} TextureSource;

static const TextureSource SOURCES[UI_TEX_COUNT] = {
   [UI_TEX_ROUND_R8]    = { texRoundR8Data,   &texRoundR8DataSize,   8,  "rund-r8"        },
   [UI_TEX_ROUND_R16]   = { texRoundR16Data,  &texRoundR16DataSize,  16, "rund-r16"       },
   [UI_TEX_ROUND_R28]   = { texRoundR28Data,  &texRoundR28DataSize,  28, "rund-r28"       },
   [UI_TEX_CORNER_R14]  = { texCornerR14Data, &texCornerR14DataSize, 7,  "ecke-r14"       },
   [UI_TEX_CORNER_R16]  = { texCornerR16Data, &texCornerR16DataSize, 8,  "ecke-r16"       },
   [UI_TEX_RING_R20_W3] = { texRingR20W3Data, &texRingR20W3DataSize, 20, "ring-r20-w3"    },
   [UI_TEX_RING_R23_W4] = { texRingR23W4Data, &texRingR23W4DataSize, 24, "ring-r23-w4"    },
   [UI_TEX_RING_R25_W4] = { texRingR25W4Data, &texRingR25W4DataSize, 24, "ring-r25-w4"    },
   [UI_TEX_RING_R34_W4] = { texRingR34W4Data, &texRingR34W4DataSize, 36, "ring-r34-w4"    },
   [UI_TEX_GLOW_R23]    = { texGlowR23Data,   &texGlowR23DataSize,   95, "schein-r23"     },
   [UI_TEX_CIRCLE_64]   = { texCircle64Data,  &texCircle64DataSize,  32, "kreis-64"       },
};

static GfxTexture textures[UI_TEX_COUNT];
static int        texturesLoaded;   // initUiDraw ran (the count it found is in its return value)

// Circles below this diameter are rendered at their own size instead of shrinking kreis-64: without
// mipmaps, bilinear sampling below about half size skips most of the texture's one-texel anti-aliased rim,
// and a 10-px legend dot or the 26-px knob comes out stair-stepped.
#define GENERATED_CIRCLE_MAX 32
static GfxTexture circles[GENERATED_CIRCLE_MAX];   // index = diameter; offset 0 = not made yet

// ---- lifetime ----

// Reads back two texels of rund-r8, a corner that must be clear and the middle that must be solid. The PNG
// decoder keeps alpha only for RGBA and grey+alpha images and makes anything else opaque, so a texture that
// was ever re-saved palettised would quietly turn every rounded shape into a square. Textures live in RSX
// local memory, which the PPU can read directly; ARGB in memory, so the first byte is alpha.
static void checkTextureAlpha(void)
{
   GfxTexture t = textures[UI_TEX_ROUND_R8];
   if (!t.offset || t.w < 2 || t.h < 2) return;
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   const uint8_t *base = (const uint8_t *)config.localAddress + t.offset;
   int corner = base[0];
   int middle = base[(t.h / 2) * t.pitch + (t.w / 2) * 4];
   if (corner < 64 && middle > 191) logInfo("[ui] helper-texture alpha arrived (corner %d, middle %d)\n", corner, middle);
   else logError("[ui] helper-texture alpha LOST (corner %d, middle %d): rounded shapes will draw square\n", corner, middle);
}

int initUiDraw(void)
{
   int loaded = 0;
   for (int id = 0; id < UI_TEX_COUNT; id++) {
      if (!textures[id].offset) textures[id] = loadGfxTextureMem(SOURCES[id].data, *SOURCES[id].size);
      if (textures[id].offset) loaded++;
      else logError("[ui] helper texture %s did not load; it is drawn flat (or not at all) instead\n", SOURCES[id].name);
   }
   if (!texturesLoaded) {
      texturesLoaded = 1;
      checkTextureAlpha();
      logInfo("[ui] %d of %d helper textures loaded\n", loaded, (int)UI_TEX_COUNT);
   }
   return loaded;
}

void termUiDraw(void)
{
   finishGfx();   // the RSX may still be reading any of these from the last frame
   for (int id = 0; id < UI_TEX_COUNT; id++) freeGfxTexture(&textures[id]);
   for (int d = 0; d < GENERATED_CIRCLE_MAX; d++) freeGfxTexture(&circles[d]);
   texturesLoaded = 0;
}

void uiLogVram(const char *where)
{
   // KB is fine-grained enough: the smallest thing that comes and goes, a thumbnail, is 225 KB
   logInfo("[vram] %s: used %u KB, free %u KB, largest free block %u KB\n", where ? where : "-",
           (unsigned)(getUsedVram() / 1024u), (unsigned)(getFreeVram() / 1024u),
           (unsigned)(getLargestFreeBlock() / 1024u));
}

// ---- the textures ----

GfxTexture uiGetTexture(UiTexture id)
{
   GfxTexture none;
   memset(&none, 0, sizeof none);
   return ((int)id >= 0 && id < UI_TEX_COUNT) ? textures[id] : none;
}

int uiGetTextureCaps(UiTexture id)
{
   return ((int)id >= 0 && id < UI_TEX_COUNT) ? SOURCES[id].caps : 0;
}

// A NineSlice needs the whole texture named as its source region: a zeroed SpriteRegion is not "all of it"
// there (unlike initImage) but a zero-sized one, and yields garbage UVs. The tint goes in AFTER init,
// which resets it to white.
static void drawSliced(GfxTexture tex, int caps, int x, int y, int w, int h, float scale, uint32_t tint)
{
   NineSlice slice;
   SpriteRegion whole = { 0, 0, tex.w, tex.h };
   initNineSliceScaled(&slice, tex, x, y, w, h, whole, caps, caps, scale);
   slice.tint = tint;
   drawNineSlice(&slice);
}

void uiDrawNineSlice(UiTexture id, int x, int y, int w, int h, float scale, uint32_t tint)
{
   if ((int)id < 0 || id >= UI_TEX_COUNT || !textures[id].offset || w <= 0 || h <= 0) return;
   drawSliced(textures[id], SOURCES[id].caps, x, y, w, h, scale, tint);
}

// ---- fills ----

void uiFillRect(int x, int y, int w, int h, uint32_t argb)
{
   if (w > 0 && h > 0) fillGfxRectangle(x, y, w, h, argb);
}

static float clampUnit(float value)
{
   if (value < 0.0f) return 0.0f;
   if (value > 1.0f) return 1.0f;
   return value;
}

// One band of two triangles from position a to b (0..1 along the gradient), colour `from` -> `to`. The RSX
// interpolates vertex colours in straight alpha, so a "transparent" end must keep the RGB of the colour it
// fades from - 0x00000000 would fade through grey.
static void fillGradientBand(int x, int y, int w, int h, int vertical, float a, float b, uint32_t from, uint32_t to)
{
   if ((from >> 24) == 0) from = to & 0x00FFFFFFu;
   if ((to >> 24) == 0)   to   = from & 0x00FFFFFFu;
   if ((from >> 24) == 0 && (to >> 24) == 0) return;   // clear to clear: nothing to draw

   float start = vertical ? (float)y : (float)x;
   float span  = vertical ? (float)h : (float)w;
   float bandStart = start + clampUnit(a) * span;
   float bandEnd   = start + clampUnit(b) * span;
   if (bandEnd <= bandStart) return;

   float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
   if (vertical) {
      drawGfxTriangle(x0, bandStart, from, x1, bandStart, from, x0, bandEnd, to);
      drawGfxTriangle(x1, bandStart, from, x1, bandEnd, to, x0, bandEnd, to);
   } else {
      drawGfxTriangle(bandStart, y0, from, bandEnd, y0, to, bandStart, y1, from);
      drawGfxTriangle(bandEnd, y0, to, bandEnd, y1, to, bandStart, y1, from);
   }
}

// As CSS does it: the first stop's colour fills the box up to the first stop, the last one's from the
// last stop on, so a gradient copied from the design's CSS covers exactly what it covers there.
static void fillGradient(int x, int y, int w, int h, const UiGradientStop *stops, int count, int vertical)
{
   if (!stops || count < 2 || w <= 0 || h <= 0) return;
   fillGradientBand(x, y, w, h, vertical, 0.0f, stops[0].at, stops[0].argb, stops[0].argb);
   for (int i = 0; i + 1 < count; i++)
      fillGradientBand(x, y, w, h, vertical, stops[i].at, stops[i + 1].at, stops[i].argb, stops[i + 1].argb);
   fillGradientBand(x, y, w, h, vertical, stops[count - 1].at, 1.0f, stops[count - 1].argb, stops[count - 1].argb);
}

void uiFillGradientV(int x, int y, int w, int h, const UiGradientStop *stops, int count)
{
   fillGradient(x, y, w, h, stops, count, 1);
}

void uiFillGradientH(int x, int y, int w, int h, const UiGradientStop *stops, int count)
{
   fillGradient(x, y, w, h, stops, count, 0);
}

void uiFillGradient2V(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
   UiGradientStop stops[2];
   stops[0].at = 0.0f; stops[0].argb = top;
   stops[1].at = 1.0f; stops[1].argb = bottom;
   fillGradient(x, y, w, h, stops, 2, 1);
}

void uiFillRounded(int x, int y, int w, int h, int radius, uint32_t argb)
{
   if (w <= 0 || h <= 0) return;
   int shorter = w < h ? w : h;
   if (radius > shorter / 2) radius = shorter / 2;
   if (radius <= 0) { fillGfxRectangle(x, y, w, h, argb); return; }

   // only ever scale a corner DOWN (UEBERGABE 4): the smallest texture whose radius covers the one asked
   // for. That keeps every scale >= 0.56 (radius 9 on rund-r16) except for radii under 5 on rund-r8 - the
   // 6-8 px bars - where the edge is too short to show any stair-stepping.
   UiTexture id = radius <= 8 ? UI_TEX_ROUND_R8 : radius <= 16 ? UI_TEX_ROUND_R16 : UI_TEX_ROUND_R28;
   if (!textures[id].offset) { fillGfxRectangle(x, y, w, h, argb); return; }
   drawSliced(textures[id], SOURCES[id].caps, x, y, w, h, (float)radius / (float)SOURCES[id].caps, argb);
}

void uiFillPill(int x, int y, int w, int h, uint32_t argb)
{
   int shorter = w < h ? w : h;
   uiFillRounded(x, y, w, h, shorter / 2, argb);
}

// ---- images ----

void uiDrawImageCorners(int x, int y, int w, int h, int radius, uint32_t behindArgb)
{
   if (radius <= 0 || w <= 0 || h <= 0) return;
   GfxTexture mask = textures[radius <= 14 ? UI_TEX_CORNER_R14 : UI_TEX_CORNER_R16];
   if (!mask.offset) return;   // square corners, the way thumbnails looked until now
   int c = radius;
   if (c * 2 > w) c = w / 2;
   if (c * 2 > h) c = h / 2;
   // the mask is authored for the top-left corner; swapping u0/u1 or v0/v1 mirrors it onto the others.
   // Four quads of one texture in a row are a single batch.
   drawGfxTexture(x,         y,         c, c, mask, 0.0f, 0.0f, 1.0f, 1.0f, behindArgb, GFX_FILTER_LINEAR);
   drawGfxTexture(x + w - c, y,         c, c, mask, 1.0f, 0.0f, 0.0f, 1.0f, behindArgb, GFX_FILTER_LINEAR);
   drawGfxTexture(x,         y + h - c, c, c, mask, 0.0f, 1.0f, 1.0f, 0.0f, behindArgb, GFX_FILTER_LINEAR);
   drawGfxTexture(x + w - c, y + h - c, c, c, mask, 1.0f, 1.0f, 0.0f, 0.0f, behindArgb, GFX_FILTER_LINEAR);
}

// ---- focus ----

// The glow texture is a blurred rectangle whose edge sits 72 px inside the texture's own (caps 95 = 72 + its
// radius 23). With the target 81 px outside the card, that edge falls 9 px outside the card - on the ring's
// outer edge - and the glow fades out over the 72 px beyond it.
#define GLOW_OUTSET 81

void uiDrawGlow(int x, int y, int w, int h, float scale, uint32_t tint)
{
   GfxTexture glow = textures[UI_TEX_GLOW_R23];
   if (!glow.offset || w <= 0 || h <= 0 || scale <= 0.0f) return;
   int outset = (int)(GLOW_OUTSET * scale + 0.5f);
   drawSliced(glow, SOURCES[UI_TEX_GLOW_R23].caps, x - outset, y - outset, w + 2 * outset, h + 2 * outset, scale, tint);
}

void uiDrawFocusGlow(int x, int y, int w, int h, uint32_t tint)
{
   uiDrawGlow(x, y, w, h, 1.0f, tint);
}

typedef struct {
   UiTexture texture;
   int       outset;   // the ring's outer edge, px outside the box (= gap + ring width)
   int       width;    // the ring's width, for the flat fallback
} RingStyle;

// Each ring texture has its ring at the texture's very edge, and its outer radius is the box's radius plus
// the outset - so drawn at the box + outset, the ring keeps the design's gap to the box all the way round.
static const RingStyle RINGS[] = {
   [UI_RING_THUMB]   = { UI_TEX_RING_R23_W4, 9, 4 },   // radius 14 + 5 gap + 4 ring = 23
   [UI_RING_CARD]    = { UI_TEX_RING_R25_W4, 9, 4 },   // 16 + 5 + 4 = 25
   [UI_RING_ROW]     = { UI_TEX_RING_R34_W4, 8, 4 },   // 26 + 4 + 4 = 34
   [UI_RING_SEGMENT] = { UI_TEX_RING_R20_W3, 6, 3 },   // 14 + 3 + 3 = 20
};
#define RING_STYLE_COUNT ((int)(sizeof RINGS / sizeof RINGS[0]))

void uiDrawFocusRing(int x, int y, int w, int h, UiRing ring, uint32_t tint)
{
   if ((int)ring < 0 || (int)ring >= RING_STYLE_COUNT || w <= 0 || h <= 0) return;
   const RingStyle *style = &RINGS[ring];
   int rx = x - style->outset, ry = y - style->outset;
   int rw = w + 2 * style->outset, rh = h + 2 * style->outset;
   GfxTexture tex = textures[style->texture];
   if (!tex.offset) { strokeGfxRectangle(rx, ry, rw, rh, style->width, tint); return; }
   drawSliced(tex, SOURCES[style->texture].caps, rx, ry, rw, rh, 1.0f, tint);
}

// ---- text on shapes ----

void uiDrawLabelAt(const Label *label, int x, int y)
{
   if (!label || label->tt.tex.w <= 0) return;
   // exactly drawLabel's draw: the colour is in the texture, the tint only carries the label's alpha
   uint32_t tint = (label->color & 0xFF000000u) | 0x00FFFFFFu;
   drawGfxTexture(x, y, label->tt.tex.w, label->tt.tex.h, label->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, tint,
                  GFX_FILTER_NEAREST);
}

int uiGetTextBoxWidth(const Label *label, int padX)
{
   int textW = (label && label->tt.tex.w > 0) ? label->tt.tex.w : 0;
   return textW + 2 * padX;
}

int uiDrawTextBox(int x, int y, int h, int padX, int radius, const Label *label, uint32_t fillArgb)
{
   int w = uiGetTextBoxWidth(label, padX);
   uiFillRounded(x, y, w, h, radius, fillArgb);
   if (label && label->tt.tex.w > 0) uiDrawLabelAt(label, x + padX, uiLabelYForCenter(label, y, h));
   return w;
}

int uiDrawBadge(int x, int y, const Label *label, uint32_t fillArgb)
{
   return uiDrawTextBox(x, y, UI_BADGE_H, UI_BADGE_PAD_X, UI_RADIUS_BADGE, label, fillArgb);
}

int uiDrawChip(int x, int y, int h, int padX, const Label *label, uint32_t fillArgb)
{
   return uiDrawTextBox(x, y, h, padX, h / 2, label, fillArgb);
}

// ---- bars and dots ----

void uiDrawProgressBar(int x, int y, int w, int h, float fraction, uint32_t trackArgb, uint32_t fillArgb,
                       int radius)
{
   if (w <= 0 || h <= 0) return;
   int fillW = (int)((float)w * clampUnit(fraction) + 0.5f);
   if (radius > 0) {
      // the fill goes over the track: two rounded pieces cannot be butted against each other cleanly
      uiFillRounded(x, y, w, h, radius, trackArgb);
      if (fillW > 0) uiFillRounded(x, y, fillW, h, radius, fillArgb);
   } else {
      // side by side, so a translucent fill is not blended over the track as well
      if (fillW > 0) fillGfxRectangle(x, y, fillW, h, fillArgb);
      if (fillW < w) fillGfxRectangle(x + fillW, y, w - fillW, h, trackArgb);
   }
}

// A white disc of exactly `diameter` px with an anti-aliased rim: every pixel gets the share of an 8x8 grid
// of sample points that falls inside the circle. Uploaded once per diameter and kept until termUiDraw.
static GfxTexture makeCircle(int diameter)
{
   static uint32_t pixels[GENERATED_CIRCLE_MAX * GENERATED_CIRCLE_MAX];
   float radius = (float)diameter * 0.5f;
   for (int py = 0; py < diameter; py++) {
      for (int px = 0; px < diameter; px++) {
         int inside = 0;
         for (int sy = 0; sy < 8; sy++) {
            float dy = (float)py + ((float)sy + 0.5f) / 8.0f - radius;
            for (int sx = 0; sx < 8; sx++) {
               float dx = (float)px + ((float)sx + 0.5f) / 8.0f - radius;
               if (dx * dx + dy * dy <= radius * radius) inside++;
            }
         }
         uint32_t alpha = (uint32_t)((inside * 255 + 32) / 64);
         pixels[py * diameter + px] = (alpha << 24) | 0x00FFFFFFu;   // ARGB, the layout uploadGfxTexture takes
      }
   }
   GfxTexture tex;
   memset(&tex, 0, sizeof tex);
   uint32_t offset = uploadGfxTexture(pixels, diameter, diameter, diameter * 4);
   if (!offset) return tex;
   tex.offset = offset;
   tex.w      = diameter;
   tex.h      = diameter;
   tex.pitch  = (diameter * 4 + 63) & ~63;
   return tex;
}

void uiFillCircle(int x, int y, int diameter, uint32_t argb)
{
   if (diameter <= 0) return;
   if (diameter < GENERATED_CIRCLE_MAX) {
      if (!circles[diameter].offset) circles[diameter] = makeCircle(diameter);
      if (circles[diameter].offset) {
         // 1:1, so the rim is exactly as rendered (LINEAR only matters at 720p, where everything shrinks)
         drawGfxTexture(x, y, diameter, diameter, circles[diameter], 0.0f, 0.0f, 1.0f, 1.0f, argb, GFX_FILTER_LINEAR);
         return;
      }
   } else if (textures[UI_TEX_CIRCLE_64].offset) {
      drawGfxTexture(x, y, diameter, diameter, textures[UI_TEX_CIRCLE_64], 0.0f, 0.0f, 1.0f, 1.0f, argb, GFX_FILTER_LINEAR);
      return;
   }
   fillGfxCircle(x + diameter / 2, y + diameter / 2, diameter / 2, argb);   // out of VRAM: a hard-edged disc
}

// ---- outlined text ----

static const int OUTLINE_DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
static const int OUTLINE_DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

// the 8 offset copies of one texture; consecutive draws of the same texture and filter are one batch
static void stampOutline(const GfxTexture *tex, int x, int y, uint32_t outlineArgb, int radius)
{
   for (int i = 0; i < 8; i++)
      drawGfxTexture(x + OUTLINE_DX[i] * radius, y + OUTLINE_DY[i] * radius, tex->w, tex->h, *tex,
                     0.0f, 0.0f, 1.0f, 1.0f, outlineArgb, GFX_FILTER_NEAREST);
}

void uiDrawLabelOutlined(const Label *label, uint32_t outlineArgb, int radius)
{
   if (!label || label->tt.tex.w <= 0) return;
   if (radius > 0) stampOutline(&label->tt.tex, label->x, label->y, outlineArgb, radius);
   drawLabel(label);   // same texture and filter as the stamps, so still the same batch
}

// ---- fixed-width numbers ----

static int digitIndex(char c)
{
   const char *hit = c ? strchr(UI_DIGIT_CHARS, c) : NULL;
   return hit ? (int)(hit - UI_DIGIT_CHARS) : -1;
}

static int isDigitChar(char c) { return c >= '0' && c <= '9'; }

void uiInitDigits(UiDigits *digits, UiWeight weight, int size)
{
   memset(digits, 0, sizeof *digits);
   digits->font = uiGetFont(weight);
   digits->size = size;
   float widest = 0.0f;
   for (int i = 0; i < UI_DIGIT_COUNT; i++) {
      char c = UI_DIGIT_CHARS[i];
      // measureFontChar does not check that the font is open (measureFontText does), so ask first
      float advance = digits->font->open ? measureFontChar(digits->font, size, (uint32_t)(unsigned char)c)
                                         : (float)size * 0.6f;
      digits->advance[i] = (int)(advance + 0.5f);
      if (isDigitChar(c) && advance > widest) widest = advance;
      if (c == ' ') continue;   // a space has nothing to draw, only its advance
      char text[2];
      text[0] = c;
      text[1] = '\0';
      // white, so drawing can tint it any colour; one tiny texture per character, rendered once here. Two em
      // is wider than any of these characters - and the player inits this per video, so the renderer should
      // not clear a 1024-px surface (AUTO) fourteen times for it. The rows come out the same either way.
      initLabel(&digits->glyphs[i], digits->font, 0, 0, size * 2 + 8, AUTO, size, 0xFFFFFFFF, TEXT_NOWRAP, text);
   }
   digits->digitCell = (int)ceilf(widest);
}

void uiFreeDigits(UiDigits *digits)
{
   for (int i = 0; i < UI_DIGIT_COUNT; i++) freeLabel(&digits->glyphs[i]);
}

int uiGetDigitsWidth(const UiDigits *digits, const char *text)
{
   int width = 0;
   for (const char *p = text ? text : ""; *p; p++) {
      int i = digitIndex(*p);
      if (i >= 0) width += isDigitChar(*p) ? digits->digitCell : digits->advance[i];
   }
   return width;
}

// Walks text once for one pass: outline stamps (outlineRadius > 0) or the characters themselves. A digit is
// centred in its cell, so a narrow '1' does not leave its gap all on one side.
static void drawDigitsPass(const UiDigits *digits, int x, int y, const char *text, uint32_t tint, int outlineRadius)
{
   for (const char *p = text ? text : ""; *p; p++) {
      int i = digitIndex(*p);
      if (i < 0) continue;
      int step = isDigitChar(*p) ? digits->digitCell : digits->advance[i];
      const GfxTexture *tex = &digits->glyphs[i].tt.tex;
      if (tex->w > 0) {
         int gx = isDigitChar(*p) ? x + (step - tex->w) / 2 : x;
         if (outlineRadius > 0) stampOutline(tex, gx, y, tint, outlineRadius);
         else drawGfxTexture(gx, y, tex->w, tex->h, *tex, 0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_NEAREST);
      }
      x += step;
   }
}

void uiDrawDigits(const UiDigits *digits, int x, int y, const char *text, uint32_t color)
{
   drawDigitsPass(digits, x, y, text, color, 0);
}

void uiDrawDigitsOutlined(const UiDigits *digits, int x, int y, const char *text, uint32_t color,
                          uint32_t outlineArgb, int radius)
{
   // every outline before any fill: a neighbour's outline drawn later would bite into a digit
   if (radius > 0) drawDigitsPass(digits, x, y, text, outlineArgb, radius);
   drawDigitsPass(digits, x, y, text, color, 0);
}

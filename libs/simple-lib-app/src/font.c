// font - truetype font rendering via libfont/freetype
#include "font.h"
#include <cell/font.h>
#include <cell/fontFT.h>
#include <cell/sysmodule.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gfx.h"
#include "colors.h"
#include "dbg.h"
#include "vfs.h"

static const int FONT_MAX_RENDER_W = 1024;
static const size_t FONT_MAX_SURFACE_BYTES = 16u * 1024u * 1024u;   // sanity cap for the rasterize() scratch buffer

static const CellFontLibrary *fontLib = NULL;
static CellFontRenderer       fontRenderer;
static int                    fontInited = 0;

static void *fontMalloc(void *obj, uint32_t size) { (void)obj; return malloc(size); }
static void  fontFree(void *obj, void *p) { (void)obj; free(p); }
static void *fontRealloc(void *obj, void *p, uint32_t size) { (void)obj; return realloc(p, size); }
static void *fontCalloc(void *obj, uint32_t n, uint32_t sz) { (void)obj; return calloc(n, sz); }

int initFont(void)
{
   if (fontInited) return 0;

   cellSysmoduleLoadModule(CELL_SYSMODULE_FONT);
   cellSysmoduleLoadModule(CELL_SYSMODULE_FREETYPE);
   cellSysmoduleLoadModule(CELL_SYSMODULE_FONTFT);

   static uint32_t fontFileCache[256 * 1024 / sizeof(uint32_t)];
   // Slots for user fonts opened via openFontFile/openFontMemory. CellFontConfig_initialize
   // defaults userFontEntryMax to 0, so without this any user-font open returns
   // CELL_FONT_ERROR_FONT_OPEN_MAX (0x8054000d). (System fonts don't use these slots.)
   static CellFontEntry userFontEntries[8];

   CellFontConfig fconfig;
   CellFontConfig_initialize(&fconfig);
   fconfig.FileCache.buffer = fontFileCache;
   fconfig.FileCache.size   = sizeof(fontFileCache);
   fconfig.flags = 0;
   fconfig.userFontEntryMax  = 8;
   fconfig.userFontEntrys    = userFontEntries;
   if (cellFontInit(&fconfig) != CELL_OK) return -1;

   CellFontLibraryConfigFT config;
   CellFontLibraryConfigFT_initialize(&config);
   config.MemoryIF.Object  = NULL;
   config.MemoryIF.Malloc  = fontMalloc;
   config.MemoryIF.Free    = fontFree;
   config.MemoryIF.Realloc = fontRealloc;
   config.MemoryIF.Calloc  = fontCalloc;

   if (cellFontInitLibraryFreeType(&config, &fontLib) != CELL_OK) return -1;

   CellFontRendererConfig rconfig;
   CellFontRendererConfig_initialize(&rconfig);
   CellFontRendererConfig_setAllocateBuffer(&rconfig, 1024 * 64, 0);
   if (cellFontCreateRenderer(fontLib, &rconfig, &fontRenderer) != CELL_OK) return -1;

   fontInited = 1;
   return 0;
}

void termFont(void)
{
   if (!fontInited) return;
   cellFontDestroyRenderer(&fontRenderer);
   cellFontEndLibrary(fontLib);
   cellFontEnd();
   cellSysmoduleUnloadModule(CELL_SYSMODULE_FONTFT);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_FREETYPE);
   cellSysmoduleUnloadModule(CELL_SYSMODULE_FONT);
   fontInited = 0;
}

Font openSystemFont(int type)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();

   int fontType;
   switch (type) {
      case FONT_POP:       fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_LATIN_SET; break;
      case FONT_GOTHIC_JP: fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_JP_SET; break;
      case FONT_SANS:      fontType = CELL_FONT_TYPE_DEFAULT_SANS_SERIF; break;
      case FONT_SERIF:     fontType = CELL_FONT_TYPE_DEFAULT_SERIF; break;
      default:             fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_LATIN_SET; break;
   }

   CellFontType ft;
   ft.type = fontType;
   ft.map  = CELL_FONT_MAP_UNICODE;

   if (cellFontOpenFontset(fontLib, &ft, &f.font) == CELL_OK) {
      f.open = 1;
      cellFontSetResolutionDpi(&f.font, 72, 72);
   }

   return f;
}

// cellFont caches opened faces by the `uniqueId` passed to cellFontOpenFont*; if two different
// fonts share an id (the old hardcoded 0) the second open returns the FIRST cached face. Hand out a
// fresh id per open so every font is distinct (e.g. the dialogue font and the in-game chat font).
static int nextFontUid(void) { static int uid = 1; return uid++; }

// Parse the SFNT (TrueType/OpenType) header for the hhea ascender/descender + head unitsPerEm, which is
// what FreeType (and hence Ren'Py, ftfont.pyx) uses for the line height (ascender - descender). cellFont's
// CellFontHorizontalLayout instead uses the OS/2 usWin* metrics, which over-space some OTF fonts. SFNT is
// big-endian. Fills f->unitsPerEm/hheaAscent/hheaDescent; unitsPerEm stays 0 if the tables aren't found.
static uint16_t sfntU16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  sfntS16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t sfntU32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void parseSfntMetrics(Font *f, const uint8_t *buf, uint32_t size)
{
   f->unitsPerEm = 0; f->hheaAscent = 0; f->hheaDescent = 0;
   if (!buf || size < 12) return;
   uint16_t numTables = sfntU16(buf + 4);     // offset table: u32 tag, u16 numTables, ...
   uint32_t headOff = 0, hheaOff = 0;
   for (uint16_t i = 0; i < numTables; i++)
   {
      uint32_t rec = 12u + (uint32_t)i * 16u;   // each table record: tag(4) checksum(4) offset(4) length(4)
      if (rec + 16u > size) break;
      const uint8_t *tag = buf + rec;
      uint32_t off = sfntU32(buf + rec + 8);
      if (off > size) continue;   // reject bogus offsets before they can overflow the checks below
      if (tag[0]=='h'&&tag[1]=='e'&&tag[2]=='a'&&tag[3]=='d') headOff = off;
      else if (tag[0]=='h'&&tag[1]=='h'&&tag[2]=='e'&&tag[3]=='a') hheaOff = off;
   }
   int upm = (headOff && headOff + 20u <= size) ? sfntU16(buf + headOff + 18) : 0;   // head.unitsPerEm @ +18
   if (hheaOff && hheaOff + 10u <= size && upm > 0)
   {
      f->hheaAscent  = sfntS16(buf + hheaOff + 4);   // hhea.ascender  @ +4
      f->hheaDescent = sfntS16(buf + hheaOff + 6);   // hhea.descender @ +6
      if (f->hheaAscent != 0 || f->hheaDescent != 0) f->unitsPerEm = upm;
   }
}

Font openFontFile(const char *path)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();

   // size via path-stat (the VFS has no fstat-by-handle); then open + read.
   VfsStat st;
   if (statPath(path, &st) != 0) return f;
   uint32_t size = (uint32_t)st.size;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return f;

   void *buf = malloc(size);
   if (!buf) { closeFs(&file); return f; }

   readFs(&file, buf, size);
   closeFs(&file);

   if (cellFontOpenFontMemory(fontLib, buf, size, 0, nextFontUid(), &f.font) == CELL_OK) {
      f.open = 1;
      f.buffer = buf;  // cellFont keeps reading buf for the font's life; closeFont frees it
      cellFontSetResolutionDpi(&f.font, 72, 72);
      parseSfntMetrics(&f, buf, size);   // hhea/head metrics for the faithful (Ren'Py) line height
   } else {
      free(buf);
   }

   return f;
}

Font openFontMemory(const void *buf, uint32_t size)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();
   if (!buf || size == 0) return f;

   // cellFont keeps reading the buffer for the font's lifetime, so own a private copy.
   void *own = malloc(size);
   if (!own) return f;
   memcpy(own, buf, size);

   int ret = cellFontOpenFontMemory(fontLib, own, size, 0, nextFontUid(), &f.font);
   if (ret == CELL_OK) {
      f.open = 1;
      f.buffer = own;  // closeFont frees it
      cellFontSetResolutionDpi(&f.font, 72, 72);
      parseSfntMetrics(&f, (const uint8_t *)own, size);   // hhea/head metrics for the faithful line height
   } else {
      logError("[font] cellFontOpenFontMemory failed: 0x%x (size=%u)\n", ret, size);
      free(own);
   }

   return f;
}

void closeFont(Font *f)
{
   if (f->open) {
      cellFontCloseFont(&f->font);
      f->open = 0;
   }
   free(f->buffer);  // backing memory for openFontFile; NULL (safe) for system fonts
   f->buffer = NULL;
}

void setFontMetrics(Font *f, FontMetricsMode mode, float grid)
{
   f->metricsMode = (int)mode;
   f->metricsGrid = grid > 0.0f ? grid : 0.0f;
}

// Applies the font's advance model to a raw advance (see FontMetricsMode). The small
// epsilon keeps an exact multiple from ceiling up a step due to float error.
static float quantAdvance(const Font *f, float adv)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || f->metricsGrid <= 0.0f) return adv;
   float cells = ceilf(adv / f->metricsGrid - 0.001f);
   return cells * f->metricsGrid;
}

// Kerning between two glyphs at the current scale, per the font's metrics mode.
// NATIVE mode reports 0 (fractional-advance stacks don't kern); GRID_CEIL floors the
// offset to the grid, matching classic stacks' integer `delta.x >> 6`.
static float fontKern(Font *f, uint32_t prev, uint32_t code)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || prev == 0) return 0.0f;
   CellFontKerning k;
   if (cellFontGetKerning(&f->font, prev, code, &k) != CELL_OK) return 0.0f;
   if (k.offsetX == 0.0f) return 0.0f;
   float g = f->metricsGrid > 0.0f ? f->metricsGrid : 1.0f;
   return floorf(k.offsetX / g + 0.001f) * g;
}

// decodes one utf-8 codepoint, advances *pp past it. returns codepoint.
static uint32_t decodeUtf8(const uint8_t **pp)
{
   const uint8_t *p = *pp;
   uint32_t c = *p;
   if (c < 0x80) { (*pp)++; return c; }
   if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      *pp += 2;
      return ((c & 0x1F) << 6) | (p[1] & 0x3F);
   }
   if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      *pp += 3;
      return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
   }
   if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      *pp += 4;
      return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
   }
   (*pp)++;
   return c;
}

// (TEE Vanced V3.1.0) right-to-left text - see "right-to-left text" further down
static int   textHasRtl(const char *text);
static float measureRtlText(Font *f, int size, const char *text);

float measureFontText(Font *f, int size, const char *text)
{
   if (!f->open || !text) return 0.0f;
   if (textHasRtl(text)) return measureRtlText(f, size, text);   // Arabic is as wide as the joined forms it is drawn in
   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);

   float w = 0.0f;
   uint32_t prev = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p) {
      uint32_t code = decodeUtf8(&p);
      CellFontGlyphMetrics metrics;
      if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
         w += quantAdvance(f, metrics.Horizontal.advance) + fontKern(f, prev, code);
      else
         w += fsize;
      prev = code;
   }
   return w;
}

float measureFontChar(Font *f, int size, uint32_t code)
{
   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);
   CellFontGlyphMetrics metrics;
   if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
      return quantAdvance(f, metrics.Horizontal.advance);
   return fsize;
}

// inline style tags: {i} {b} {color=#rrggbb} (+ {{ and [[ escapes)
// Lightweight inline markup for styled runs inside one string. Text is pre-parsed into
// styled glyph items so tag handling stays separate from the (already fiddly)
// wrapping/layout logic, which then just walks the item array.
typedef struct { uint32_t code; uint8_t italic; uint8_t bold; uint32_t color; } GlyphItem;

// Parses text into items[] (<= maxItems). baseColor is the default colour. Nested {i}/{b}
// are counted; {color} uses a small stack. Unknown tags ({w},{p},{size=..},...) are skipped.
// rawMode disables all of the above - every byte (including '{'/'[') is literal content, for
// text that comes from outside the app (file contents, filenames, ...) rather than authored UI copy.
static int parseStyledText(const char *text, uint32_t baseColor, GlyphItem *items, int maxItems, int rawMode)
{
   int italic = 0, bold = 0;
   uint32_t colorStack[8];
   int colorTop = 0;
   colorStack[0] = baseColor;

   int n = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p && n < maxItems) {
      if (!rawMode && p[0] == '{' && p[1] == '{') {   // literal brace
         items[n].code = '{'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '[' && p[1] == '[') {   // literal bracket
         items[n].code = '['; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '{') {                  // a tag
         const uint8_t *q = p + 1;
         while (*q && *q != '}') q++;
         int len = (int)(q - (p + 1));
         const char *t = (const char *)(p + 1);
         if      (len == 1 && t[0] == 'i') italic++;
         else if (len == 2 && t[0] == '/' && t[1] == 'i') { if (italic > 0) italic--; }
         else if (len == 1 && t[0] == 'b') bold++;
         else if (len == 2 && t[0] == '/' && t[1] == 'b') { if (bold > 0) bold--; }
         else if (len >= 7 && strncmp(t, "color=#", 7) == 0) {
            // colour literal parsing lives in colors.h (shared with manifest parsing etc.)
            if (colorTop < 7) { colorTop++; colorStack[colorTop] = parseColorSpan(t + 7, len - 7, colorStack[colorTop - 1]); }
         }
         else if (len == 6 && strncmp(t, "/color", 6) == 0) { if (colorTop > 0) colorTop--; }
         // other tags carry no glyph: skip
         p = (*q == '}') ? q + 1 : q;
         continue;
      }
      if (p[0] == '\n') {
         items[n].code = '\n'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p++; continue;
      }
      uint32_t code = decodeUtf8(&p);
      items[n].code = code; items[n].italic = (uint8_t)(italic > 0); items[n].bold = (uint8_t)(bold > 0); items[n].color = colorStack[colorTop];
      n++;
   }
   return n;
}

static float getGlyphAdvance(Font *f, uint32_t code, float fsize)
{
   CellFontGlyphMetrics m;
   if (cellFontGetCharGlyphMetrics(&f->font, code, &m) == CELL_OK) return quantAdvance(f, m.Horizontal.advance);
   return fsize;
}

// Source-over composite of one glyph pixel (coverage a, colour cr/cg/cb) onto the surface
// pixel d = [a, r, g, b]. A plain write when the destination is empty, so the common case
// (text over a cleared surface) costs one branch.
static inline void composePixel(uint8_t *d, int a, uint8_t cr, uint8_t cg, uint8_t cb)
{
   if (a <= 0) return;
   int da = d[0];
   if (da == 0 || a >= 255) { d[0] = (uint8_t)a; d[1] = cr; d[2] = cg; d[3] = cb; return; }
   int outA = a + da * (255 - a) / 255;
   if (outA <= 0) return;
   d[1] = (uint8_t)((cr * a + d[1] * da * (255 - a) / 255) / outA);
   d[2] = (uint8_t)((cg * a + d[2] * da * (255 - a) / 255) / outA);
   d[3] = (uint8_t)((cb * a + d[3] * da * (255 - a) / 255) / outA);
   d[0] = (uint8_t)outA;
}

// Blits a rendered glyph's alpha bitmap into the surface, src-over composited.
//   dx/dy     offset from the glyph's nominal position (used by the shadow pass)
//   argb      colour; its ALPHA scales the glyph coverage (semi-transparent shadows/tags)
//   overhang  synthetic-bold strength in px (0 = regular). Faithful to FreeType's
//             FT_Bitmap_Embolden (src/base/ftbitmap.c, 8-bit gray path): each output pixel
//             is the SATURATING SUM of itself plus the `overhang` ORIGINAL pixels to its
//             left (a max would leave antialiased stroke interiors semi-transparent and
//             render thin, uneven bold), and the output is `overhang` columns wider so the
//             rightmost stroke thickens too (FreeType widens the bitmap the same way).
//             Reading from the glyph's own untouched bitmap matches FreeType's
//             right-to-left in-place pass. The caller widens the pen advance separately.
// Clipped against the surface bounds; surfBase/surfW/surfH locate the glyph's origin.
static void blitGlyph(const CellFontImageTransInfo *ti, uint8_t *surfBase, int surfW, int surfH,
                      int dx, int dy, uint32_t argb, int overhang)
{
   const uint8_t *img = (const uint8_t *)ti->Image;
   int imgW = ti->imageWidth, imgH = ti->imageHeight;
   int imgBW = ti->imageWidthByte, surfBW = ti->surfWidthByte;
   uint8_t cr = (uint8_t)((argb >> 16) & 0xFF), cg = (uint8_t)((argb >> 8) & 0xFF), cb = (uint8_t)(argb & 0xFF);
   int alpha = (int)((argb >> 24) & 0xFF);
   int off = (int)((const uint8_t *)ti->Surface - surfBase);
   int gx = (off % surfBW) / 4, gy = off / surfBW;
   int outW = imgW + overhang;
   for (int iy = 0; iy < imgH; iy++) {
      int ty = gy + iy + dy;
      if (ty < 0 || ty >= surfH) continue;
      const uint8_t *srow = img + iy * imgBW;
      uint8_t *drow = surfBase + ty * surfBW;
      for (int ix = 0; ix < outW; ix++) {
         int tx = gx + ix + dx;
         if (tx < 0 || tx >= surfW) continue;
         int a = (ix < imgW) ? srow[ix] : 0;
         for (int k = 1; k <= overhang && a < 255; k++) {
            int sx = ix - k;
            if (sx < 0) break;
            if (sx < imgW) a += srow[sx];
         }
         if (a > 255) a = 255;
         if (alpha < 255) a = a * alpha / 255;
         composePixel(drow + tx * 4, a, cr, cg, cb);
      }
   }
}

// width of the word starting at items[j0] (up to the next space/newline), including
// kerning inside the word and for the (prevCode -> first glyph) pair. Used by the wrap
// decision: a line breaks when "line + space + word" would exceed the target width.
static float getWordWidth(Font *f, const GlyphItem *items, int nItems, int j0, uint32_t prevCode, float fsize)
{
   float w = 0.0f;
   uint32_t prev = prevCode;
   for (int j = j0; j < nItems && items[j].code != ' ' && items[j].code != '\n'; j++) {
      w += getGlyphAdvance(f, items[j].code, fsize) + fontKern(f, prev, items[j].code);
      prev = items[j].code;
   }
   return w;
}

// ---- right-to-left text (TEE Vanced V3.1.0) ----
//
// cellFont draws one code point a glyph, left to right, and knows nothing of Arabic's joined letters or of lines that
// run right to left. A text with Hebrew or Arabic letters in it is therefore laid out here before it is drawn:
//  - Arabic letters take the forms they have joined to their neighbours (Arabic Presentation Forms, the lam-alef
//    ligatures); vowel marks are left out, as each would take a glyph of its own;
//  - the text is broken into lines in reading order, by the very rule the render loop uses, so a long Arabic text
//    starts at the top and goes on down;
//  - each line is put into the order it is drawn in, left to right: runs of right-to-left letters turned round, runs of
//    Latin and of numbers kept as they read (a number keeps its signs: 1.2K, 45%, 12:30), and in a paragraph that starts
//    right to left the runs themselves in reverse, the brackets of right-to-left runs mirrored;
//  - a line cut to fit (an ellipsis, or a clip) loses its end in reading order - for a right-to-left line its left
//    end, where the "..." then goes.
// The line is placed as the caller asks: left aligned in the app's menus - only the letters of a line run right to left
// (TEE: "nicht das das ganze menü mit umgewandelt wird von rechts nach links"). Text without such letters takes none of
// this; the render loop below then only skips its own wrap decisions for the lines laid out here.

enum { BIDI_N, BIDI_L, BIDI_R, BIDI_D };   // neutral, left to right, right to left, digit

static int bidiClass(uint32_t c)
{
   if (c >= '0' && c <= '9') return BIDI_D;
   if (c < 0x80) return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ? BIDI_L : BIDI_N;
   if ((c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9)) return BIDI_D;   // Arabic-Indic digits
   if (c == 0x060C || c == 0x066A || c == 0x066B || c == 0x066C) return BIDI_N;       // Arabic comma, %, decimal sign
   if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return BIDI_R;
   if (c < 0xC0 || c == 0xD7 || c == 0xF7) return BIDI_N;                             // Latin-1 signs and punctuation
   if (c >= 0x2000 && c <= 0x2BFF) return BIDI_N;                                     // punctuation, symbols, shapes
   if (c >= 0x3000 && c <= 0x303F) return BIDI_N;                                     // CJK punctuation
   return BIDI_L;
}

static int isNumberSeparator(uint32_t c) { return c == '.' || c == ',' || c == ':' || c == '/' || c == 0x066B || c == 0x066C; }
static int isNumberSign(uint32_t c)
{
   return c == '%' || c == '#' || c == '$' || c == '+' || c == '-' || c == 0x00B0 || c == 0x00A3 || c == 0x00A5 ||
          c == 0x20AC || c == 0x066A;
}

// vowel marks and other combining marks of Hebrew and Arabic: left out (a glyph of their own each, in the wrong place)
static int isRtlMark(uint32_t c)
{
   return (c >= 0x0591 && c <= 0x05BD) || c == 0x05BF || c == 0x05C1 || c == 0x05C2 || c == 0x05C4 || c == 0x05C5 ||
          c == 0x05C7 || (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
          (c >= 0x06D6 && c <= 0x06ED);
}

static int textHasRtl(const char *text)
{
   for (const uint8_t *p = (const uint8_t *)text; *p; ) {
      if (*p < 0xD6) { p++; continue; }   // nothing below U+0590 starts with a byte past 0xD5
      if (bidiClass(decodeUtf8(&p)) == BIDI_R) return 1;
   }
   return 0;
}

// the forms an Arabic letter takes: alone, joined to the letter before it, to the one after it, to both (0: none, it
// joins only to the letter before it); the four DejaVu has for Persian and Urdu letters too
typedef struct { uint16_t code, isolated, final, initial, medial; } ArabicForms;
static const ArabicForms ARABIC_FORMS[] = {
   { 0x0621, 0xFE80, 0, 0, 0 },                { 0x0622, 0xFE81, 0xFE82, 0, 0 },          { 0x0623, 0xFE83, 0xFE84, 0, 0 },
   { 0x0624, 0xFE85, 0xFE86, 0, 0 },           { 0x0625, 0xFE87, 0xFE88, 0, 0 },          { 0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },
   { 0x0627, 0xFE8D, 0xFE8E, 0, 0 },           { 0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92 }, { 0x0629, 0xFE93, 0xFE94, 0, 0 },
   { 0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98 }, { 0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C }, { 0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
   { 0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 }, { 0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 }, { 0x062F, 0xFEA9, 0xFEAA, 0, 0 },
   { 0x0630, 0xFEAB, 0xFEAC, 0, 0 },           { 0x0631, 0xFEAD, 0xFEAE, 0, 0 },          { 0x0632, 0xFEAF, 0xFEB0, 0, 0 },
   { 0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 }, { 0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 }, { 0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },
   { 0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 }, { 0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 }, { 0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
   { 0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC }, { 0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0 }, { 0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4 },
   { 0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8 }, { 0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC }, { 0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
   { 0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 }, { 0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 }, { 0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },
   { 0x0648, 0xFEED, 0xFEEE, 0, 0 },           { 0x0649, 0xFEEF, 0xFEF0, 0, 0 },          { 0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
   { 0x0679, 0xFB66, 0xFB67, 0xFB68, 0xFB69 }, { 0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59 }, { 0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D },
   { 0x0688, 0xFB88, 0xFB89, 0, 0 },           { 0x0691, 0xFB8C, 0xFB8D, 0, 0 },          { 0x0698, 0xFB8A, 0xFB8B, 0, 0 },
   { 0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91 }, { 0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95 }, { 0x06BA, 0xFB9E, 0xFB9F, 0, 0 },
   { 0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD }, { 0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF },
};
#define TATWEEL 0x0640

static const ArabicForms *arabicForms(uint32_t c)
{
   int lo = 0, hi = (int)(sizeof ARABIC_FORMS / sizeof ARABIC_FORMS[0]) - 1;
   while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (ARABIC_FORMS[mid].code == c) return &ARABIC_FORMS[mid];
      if (ARABIC_FORMS[mid].code < c) lo = mid + 1; else hi = mid - 1;
   }
   return NULL;
}

// joins to the letter after it (dual-joining) / can be joined to by the letter before it
static int joinsForward(uint32_t c)  { const ArabicForms *a = arabicForms(c); return c == TATWEEL || (a && a->initial); }
static int joinsBackward(uint32_t c) { const ArabicForms *a = arabicForms(c); return c == TATWEEL || (a && a->final); }

// the items' Arabic letters into their joined forms (in place, reading order); returns the new count (marks left out,
// lam + alef one glyph)
static int shapeArabic(GlyphItem *items, int n)
{
   int m = 0;
   for (int i = 0; i < n; i++)
      if (!isRtlMark(items[i].code)) items[m++] = items[i];
   n = m;
   uint32_t *codes = (uint32_t *)malloc(sizeof *codes * (size_t)(n + 1));
   if (!codes) return n;
   for (int i = 0; i < n; i++) codes[i] = items[i].code;
   int out = 0;
   for (int i = 0; i < n; ) {
      uint32_t c = codes[i];
      int prevJoins = i > 0 && joinsForward(codes[i - 1]);
      if (c == 0x0644 && i + 1 < n && (codes[i + 1] == 0x0622 || codes[i + 1] == 0x0623 || codes[i + 1] == 0x0625 ||
                                       codes[i + 1] == 0x0627)) {   // lam-alef: one glyph
         static const uint16_t LAM_ALEF[4][2] = { { 0xFEF5, 0xFEF6 }, { 0xFEF7, 0xFEF8 }, { 0xFEF9, 0xFEFA }, { 0xFEFB, 0xFEFC } };
         int which = codes[i + 1] == 0x0622 ? 0 : codes[i + 1] == 0x0623 ? 1 : codes[i + 1] == 0x0625 ? 2 : 3;
         items[out] = items[i];
         items[out].code = LAM_ALEF[which][prevJoins ? 1 : 0];
         out++;
         i += 2;
         continue;
      }
      const ArabicForms *a = arabicForms(c);
      items[out] = items[i];
      if (a) {
         int nextJoins = a->initial && i + 1 < n && joinsBackward(codes[i + 1]);
         if (prevJoins && nextJoins) items[out].code = a->medial;
         else if (prevJoins && a->final) items[out].code = a->final;
         else if (nextJoins) items[out].code = a->initial;
         else items[out].code = a->isolated;
      }
      out++;
      i++;
   }
   free(codes);
   return out;
}

static int paragraphIsRtl(const GlyphItem *items, int from, int n)   // its first strong letter decides
{
   for (int i = from; i < n && items[i].code != '\n'; i++) {
      int c = bidiClass(items[i].code);
      if (c == BIDI_R) return 1;
      if (c == BIDI_L) return 0;
   }
   return 0;
}

static void reverseItems(GlyphItem *items, uint8_t *cls, int from, int to)
{
   for (int a = from, b = to - 1; a < b; a++, b--) {
      GlyphItem t = items[a]; items[a] = items[b]; items[b] = t;
      uint8_t c = cls[a]; cls[a] = cls[b]; cls[b] = c;
   }
}

// one line src[0, len) into dst in the order it is drawn, left to right. cls: room for len.
static void reorderLine(const GlyphItem *src, int len, int rtl, GlyphItem *dst, uint8_t *cls)
{
   if (len <= 0) return;
   for (int k = 0; k < len; k++) cls[k] = (uint8_t)bidiClass(src[k].code);
   // a number keeps its separators and signs: 1.2K, 12:30, 45%, -5
   for (int k = 1; k + 1 < len; k++)
      if (cls[k] == BIDI_N && isNumberSeparator(src[k].code) && cls[k - 1] == BIDI_D && cls[k + 1] == BIDI_D) cls[k] = BIDI_D;
   for (int k = 1; k < len; k++)
      if (cls[k] == BIDI_N && isNumberSign(src[k].code) && cls[k - 1] == BIDI_D) cls[k] = BIDI_D;
   for (int k = len - 2; k >= 0; k--)
      if (cls[k] == BIDI_N && isNumberSign(src[k].code) && cls[k + 1] == BIDI_D) cls[k] = BIDI_D;
   for (int k = 0; k < len; k++) if (cls[k] == BIDI_D) cls[k] = BIDI_L;   // numbers read left to right
   // neutrals take the direction of the letters on both sides where they agree, else the paragraph's
   int base = rtl ? BIDI_R : BIDI_L;
   for (int k = 0; k < len; ) {
      if (cls[k] != BIDI_N) { k++; continue; }
      int e = k;
      while (e < len && cls[e] == BIDI_N) e++;
      int before = k > 0 ? cls[k - 1] : base, after = e < len ? cls[e] : base;
      int d = before == after ? before : base;
      for (int j = k; j < e; j++) cls[j] = (uint8_t)d;
      k = e;
   }
   for (int k = 0; k < len; k++) dst[k] = src[k];
   if (rtl) {   // the whole line turned round, then each left-to-right run in it back
      reverseItems(dst, cls, 0, len);
      for (int k = 0; k < len; ) {
         if (cls[k] != BIDI_L) { k++; continue; }
         int e = k;
         while (e < len && cls[e] == BIDI_L) e++;
         reverseItems(dst, cls, k, e);
         k = e;
      }
   } else {     // each right-to-left run turned round
      for (int k = 0; k < len; ) {
         if (cls[k] != BIDI_R) { k++; continue; }
         int e = k;
         while (e < len && cls[e] == BIDI_R) e++;
         reverseItems(dst, cls, k, e);
         k = e;
      }
   }
   for (int k = 0; k < len; k++) {
      if (cls[k] != BIDI_R) continue;
      switch (dst[k].code) {
      case '(': dst[k].code = ')'; break;   case ')': dst[k].code = '('; break;
      case '[': dst[k].code = ']'; break;   case ']': dst[k].code = '['; break;
      case '{': dst[k].code = '}'; break;   case '}': dst[k].code = '{'; break;
      case '<': dst[k].code = '>'; break;   case '>': dst[k].code = '<'; break;
      case 0xAB: dst[k].code = 0xBB; break; case 0xBB: dst[k].code = 0xAB; break;
      default: break;
      }
   }
}

// The items of a text with right-to-left letters, laid out as described above: joined, broken into lines (their ends
// as '\n' items), each line in drawing order. items has room for n + 8. A cut to maxWidth (TEXT_NOWRAP, or with "..."
// for TEXT_NOWRAP_ELLIPSIS) is made here, in reading order. Returns the new count; the render loop then takes the lines
// as they are. On no memory the text is left as it was (drawn unjoined, left to right - still better than nothing).
static int layoutRtl(Font *f, int size, float fsize, GlyphItem *items, int n, int maxWidth, TextWrap wrap)
{
   n = shapeArabic(items, n);
   GlyphItem *out = (GlyphItem *)malloc(sizeof *out * (size_t)(n + 8));
   uint8_t *cls = (uint8_t *)malloc((size_t)n + 8);
   if (!out || !cls) { free(out); free(cls); return n; }

   int o = 0;
   if (wrap != TEXT_WRAP && maxWidth > 0) {
      // one line: its start in reading order as far as it fits (the render loop skips '\n' in these modes)
      int m = 0;
      for (int i = 0; i < n; i++) if (items[i].code != '\n') items[m++] = items[i];
      n = m;
      float ellipsisW = 0.0f;
      if (wrap == TEXT_NOWRAP_ELLIPSIS) {
         for (int d = 0; d < 3; d++) ellipsisW += measureFontChar(f, size, '.');
         cellFontSetScalePixel(&f->font, fsize, fsize);
      }
      float px = 0.0f;
      uint32_t prev = 0;
      int cut = n, dots = 0;
      for (int i = 0; i < n; i++) {
         float adv = getGlyphAdvance(f, items[i].code, fsize) + fontKern(f, prev, items[i].code);
         if (wrap == TEXT_NOWRAP_ELLIPSIS ? (px + adv > (float)maxWidth - ellipsisW && i + 1 < n) : (px + adv > (float)maxWidth)) {
            cut = i;
            dots = wrap == TEXT_NOWRAP_ELLIPSIS;
            break;
         }
         px += adv;
         prev = items[i].code;
      }
      while (dots && cut > 0 && items[cut - 1].code == ' ') cut--;   // no gap before the "..."
      int rtl = paragraphIsRtl(items, 0, n);
      GlyphItem dot;
      memset(&dot, 0, sizeof dot);
      if (dots) { dot = items[cut > 0 ? cut - 1 : 0]; dot.code = '.'; }   // in the style of the text it ends
      if (dots && rtl) { out[o++] = dot; out[o++] = dot; out[o++] = dot; }
      reorderLine(items, cut, rtl, out + o, cls);
      o += cut;
      if (dots && !rtl) { out[o++] = dot; out[o++] = dot; out[o++] = dot; }
   } else {
      int start = 0, rtl = paragraphIsRtl(items, 0, n);
      float px = 0.0f;
      uint32_t prev = 0;
      for (int i = 0; i <= n; i++) {
         int end = i == n || items[i].code == '\n';
         if (!end && wrap == TEXT_WRAP && maxWidth > 0) {   // the render loop's own rule, in reading order
            uint32_t code = items[i].code;
            float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code);
            if (code == ' ') {
               float wordW = getWordWidth(f, items, n, i + 1, ' ', fsize);
               if (px + adv + wordW > (float)maxWidth && px > 0.0f) end = 1;   // the space ends the line, not drawn
            }
            if (!end) { px += adv; prev = code; }
         }
         if (!end) continue;
         reorderLine(items + start, i - start, rtl, out + o, cls);
         o += i - start;
         if (i < n) {
            out[o] = items[i];
            out[o].code = '\n';
            o++;
            if (items[i].code == '\n') rtl = paragraphIsRtl(items, i + 1, n);
         }
         start = i + 1;
         px = 0.0f;
         prev = 0;
      }
   }
   memcpy(items, out, sizeof *out * (size_t)o);
   free(out);
   free(cls);
   return o;
}

static float measureRtlText(Font *f, int size, const char *text)
{
   int cap = (int)strlen(text) + 1;
   GlyphItem *items = (GlyphItem *)malloc(sizeof(GlyphItem) * (size_t)cap);
   if (!items) return 0.0f;
   int n = shapeArabic(items, parseStyledText(text, 0, items, cap, 1));
   float fsize = (float)size, w = 0.0f;
   cellFontSetScalePixel(&f->font, fsize, fsize);
   uint32_t prev = 0;
   for (int i = 0; i < n; i++) {   // as measureFontText: every item, a missing glyph one em
      w += getGlyphAdvance(f, items[i].code, fsize) + fontKern(f, prev, items[i].code);
      prev = items[i].code;
   }
   free(items);
   return w;
}

// records where laid-out item `index` ends (pen x + which wrapped line it's on) for a typewriter
// reveal -- taken from the real render pass, so the map can never desync from the texture.
static void recordRevealItem(TextReveal *reveal, int index, float penX, int line)
{
   if (index < TEXT_REVEAL_MAX) { reveal->endX[index] = (int)(penX + 0.5f); reveal->line[index] = line; }
}

// The x offset of a laid-out line within the text block for a given alignment: 0 for LEFT,
// (blockW - lineW)/2 for CENTER, (blockW - lineW) for RIGHT. blockW = the longest line, so
// the longest line gets offset 0 and the block crops to it.
static float getLineStartOffset(int align, const float *lineW, float maxLineW, int lineCount, int li)
{
   if (!lineW || align == TEXT_ALIGN_LEFT || li < 0 || li >= lineCount) return 0.0f;
   float d = maxLineW - lineW[li]; if (d < 0.0f) d = 0.0f;
   return (align == TEXT_ALIGN_CENTER) ? d * 0.5f : d;
}

// rasterizes text into a CPU buffer. returns the actual content dimensions
// via outW/outH. surfW is the buffer row width (may be larger than content).
// caller must free the returned buffer. `end` (optional) receives where the
// final glyph landed (texture coords). `align` aligns each line within the block.
static uint8_t *rasterize(Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap,
                          const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode,
                          int *outW, int *outH, int *outSurfW)
{
   *outW = *outH = *outSurfW = 0;

   int sdx = 0, sdy = 0, hasShadow = 0;
   uint32_t shadowArgb = 0;
   if (shadow && (shadow->dx != 0 || shadow->dy != 0)) {
      sdx = shadow->dx; sdy = shadow->dy; shadowArgb = shadow->color; hasShadow = 1;
   }

   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);
   cellFontBindRenderer(&f->font, &fontRenderer);

   // Parse inline style tags into glyph items; layout/render walk this array.
   int cap = (int)strlen(text) + 1;
   GlyphItem *items = (GlyphItem *)malloc(sizeof(GlyphItem) * (cap + 8));   // (TEE Vanced) + room for an RTL "..."
   if (!items) return NULL;
   int nItems = parseStyledText(text, color, items, cap, rawMode);

   // (TEE Vanced V3.1.0) Hebrew or Arabic letters in it: joined, broken into lines and each line put into drawing order
   // here (layoutRtl) - the passes below then take its lines as they are instead of deciding the breaks again
   int prebroken = 0;
   for (int i = 0; i < nItems && !prebroken; i++) prebroken = bidiClass(items[i].code) == BIDI_R;
   if (prebroken) nItems = layoutRtl(f, size, fsize, items, nItems, maxWidth, wrap);

   // Baseline + line pitch the way Ren'Py does it (renpy/text/ftfont.pyx): from the FreeType (hhea)
   // ascender/descender -- ascent = ceil(ascender*size/upm), descent = floor(descender*size/upm),
   // line height = ascent - descent (descent negative). cellFont's CellFontHorizontalLayout instead
   // uses the OS/2 usWin* metrics, which are correct for fonts where they match hhea (e.g. primer.ttf)
   // but over-tall for some OTFs (toony_loons.otf: win 1.273em vs hhea 0.985em -> the box stretched).
   // Fall back to the cellFont layout only when the SFNT metrics weren't parsed (e.g. system fonts).
   float lineH, baseY;
   if (f->unitsPerEm > 0)
   {
      float asc = ceilf((float)f->hheaAscent  * fsize / (float)f->unitsPerEm);
      float dsc = floorf((float)f->hheaDescent * fsize / (float)f->unitsPerEm);   // descender is negative
      baseY = asc;
      lineH = asc - dsc;
   }
   else
   {
      CellFontHorizontalLayout layout;
      cellFontGetHorizontalLayout(&f->font, &layout);
      lineH = layout.lineHeight;
      baseY = layout.baseLineY;
   }

   // count lines for height calculation (must mirror the wrap logic in the render loop;
   // kerning resets per line, since each laid-out line is measured independently)
   int lineCount = 1;
   if (wrap == TEXT_WRAP && maxWidth > 0) {
      float px = 0.0f;
      uint32_t prev = 0;
      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;
         if (code == '\n') { lineCount++; px = 0.0f; prev = 0; continue; }
         float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code);
         if (code == ' ' && !prebroken) {
            float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
            if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineCount++; px = 0.0f; prev = 0; continue; }
         }
         px += adv;
         prev = code;
      }
   }

   if (reveal) { reveal->lineHeight = (int)(lineH + 0.5f); reveal->count = 0; }

   // For CENTER/RIGHT alignment, measure each line's width (mirroring the wrap logic above
   // and the render pass exactly -- getGlyphAdvance == the rendered pen step) so each line can be
   // offset within the block at draw time. The wrap DECISIONS still use the line-relative pen,
   // so breaks are identical; only the draw x is shifted.
   float *lineW = NULL; float maxLineW = 0.0f;
   if (align != TEXT_ALIGN_LEFT && wrap == TEXT_WRAP && maxWidth > 0 && lineCount > 0) {
      lineW = (float *)malloc(sizeof(float) * lineCount);
      if (lineW) {
         int li = 0; float px = 0.0f; uint32_t prev = 0;
         for (int i = 0; i < nItems && li < lineCount; i++) {
            uint32_t code = items[i].code;
            if (code == '\n') { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code);
            if (code == ' ' && !prebroken) {
               float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
               if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            }
            px += adv; prev = code;
         }
         if (li < lineCount) lineW[li] = px;
         for (int k = 0; k < lineCount; k++) if (lineW[k] > maxLineW) maxLineW = lineW[k];
      }
   }

   int surfW = (maxWidth > 0) ? maxWidth : FONT_MAX_RENDER_W;
   int surfH = (int)(lineH * lineCount + fsize) + 4;
   if (hasShadow && sdy > 0) surfH += sdy;   // room for the shadow below the last line

   size_t bufSize64 = (size_t)surfW * (size_t)surfH * 4u;
   if (bufSize64 == 0 || bufSize64 > FONT_MAX_SURFACE_BYTES) { free(items); free(lineW); return NULL; }
   int bufSize = (int)bufSize64;
   uint8_t *buf = (uint8_t *)malloc(bufSize);
   if (!buf) { free(items); free(lineW); return NULL; }
   memset(buf, 0, bufSize);

   CellFontRenderSurface surf;
   cellFontRenderSurfaceInit(&surf, buf, surfW * 4, 4, surfW, surfH);
   cellFontRenderSurfaceSetScissor(&surf, 0, 0, surfW, surfH);
   cellFontSetupRenderScalePixel(&f->font, fsize, fsize);

   // glyph effect state: italic via the slant shear (0.0 == genuinely no slant, safe to
   // reset). Bold is synthetic emboldening at blit time (see blitGlyph) -- the renderer's
   // weight effect is never touched, so tag-free text renders exactly like the default
   // path and the shared font object is never perturbed (a weight reset once thinned all
   // following text).
   //
   // The text is drawn in up to two passes: an optional SHADOW pass (every glyph in the
   // shadow colour, offset by sdx/sdy) and then the text pass composited over it -- the
   // classic order, so a later glyph's shadow never darkens an earlier glyph.
   int maxX = 0;
   int inkMinY = surfH, inkMaxY = 0;   // union ink box over all (non-shadow) glyphs

   float ellipsisW = 0.0f;
   if (wrap == TEXT_NOWRAP_ELLIPSIS && maxWidth > 0) {
      for (int i = 0; i < 3; i++) ellipsisW += measureFontChar(f, size, '.');
      cellFontSetScalePixel(&f->font, fsize, fsize);
      cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
   }

   for (int pass = hasShadow ? 0 : 1; pass < 2; pass++) {
      int shadowPass = (pass == 0);
      int bdx = shadowPass ? sdx : 0;
      int bdy = shadowPass ? sdy : 0;
      int curItalic = 0;
      int lineIndex = 0;   // which wrapped line the pen is on (for the reveal map)
      float penX = 0.0f;   // line-relative pen (wrap/kern/advance all use this; offset added at draw)
      float lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, 0);   // this line's draw offset
      float penY = baseY;
      uint32_t prev = 0;   // previous glyph on this line (kerning context; resets per line)

      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;

         if (code == '\n') {
            if (wrap == TEXT_WRAP) { penX = 0.0f; penY += lineH; prev = 0; lineIndex++; lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex); if (penY + lineH > (float)surfH) break; }
            if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
            continue;
         }

         float kern = fontKern(f, prev, code);
         float advance = getGlyphAdvance(f, code, fsize) + kern;

         if (maxWidth > 0 && !prebroken) {   // (TEE Vanced) laid out by layoutRtl: its lines and cut stand
            if (wrap == TEXT_WRAP) {
               if (code == ' ') {
                  float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
                  if (penX + advance + wordW > (float)maxWidth && penX > 0.0f) {
                     penX = 0.0f; penY += lineH; prev = 0; lineIndex++;   // the space wraps to the next line
                     lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex);
                     if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
                     if (penY + lineH > (float)surfH) break;
                     continue;
                  }
               }
            } else if (wrap == TEXT_NOWRAP_ELLIPSIS) {
               if (penX + advance > (float)maxWidth - ellipsisW && i + 1 < nItems) {
                  if (curItalic) {
                     cellFontSetupRenderEffectSlant(&f->font, 0.0f); curItalic = 0;
                     cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
                  }
                  for (int d = 0; d < 3; d++) {
                     CellFontImageTransInfo ti;
                     CellFontGlyphMetrics dm;
                     if (cellFontRenderCharGlyphImage(&f->font, '.', &surf, penX + lineOffX, penY, &dm, &ti) == CELL_OK) {
                        blitGlyph(&ti, buf, surfW, surfH, bdx, bdy, shadowPass ? shadowArgb : color, 0);
                        penX += dm.Horizontal.advance;
                     }
                  }
                  int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
                  break;
               }
            } else {
               if (penX + advance > (float)maxWidth) break;
            }
         }

         // apply italic slant only when it changes (bold is faux-bold at blit time)
         int wantItalic = items[i].italic;
         if (wantItalic != curItalic) {
            cellFontSetupRenderEffectSlant(&f->font, wantItalic ? 0.35f : 0.0f); curItalic = wantItalic;
            // some SDKs reset the render scale when an effect is set -- re-apply it
            cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
         }

         penX += kern;   // kerning positions THIS glyph (already counted in `advance`)

         CellFontImageTransInfo transInfo;
         CellFontGlyphMetrics metrics;
         int ret = cellFontRenderCharGlyphImage(&f->font, code, &surf, penX + lineOffX, penY, &metrics, &transInfo);
         if (ret == CELL_OK) {
            // synthetic-bold strength = pixel size / 10 (the FreeType convention; 0 at
            // tiny sizes = no embolden), and the pen advance widens by the same amount.
            int overhang = items[i].bold ? ((int)(fsize + 0.5f)) / 10 : 0;
            blitGlyph(&transInfo, buf, surfW, surfH, bdx, bdy,
                      shadowPass ? shadowArgb : items[i].color, overhang);
            penX += quantAdvance(f, metrics.Horizontal.advance) + (float)overhang;
            int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
            if (!shadowPass) {
               int goff = (int)((const uint8_t *)transInfo.Surface - buf);
               int gy = goff / transInfo.surfWidthByte;
               if (gy < inkMinY) inkMinY = gy;                                   // union ink box
               if (gy + transInfo.imageHeight > inkMaxY) inkMaxY = gy + transInfo.imageHeight;
               if (end) {
                  end->endX = endX;
                  end->endTop = gy;
                  end->endBottom = gy + transInfo.imageHeight;
                  // Line cell top = baseline - ascent. The glyph's ink top gy sits
                  // bearingY below the baseline, so baseline = gy + bearingY, and the
                  // cell top (the top edge of the line box, where a caller top-aligns an
                  // inline decoration) = gy + bearingY - baseLineY(ascent). Same units as gy.
                  end->lineTop = gy + (int)(metrics.Horizontal.bearingY + 0.5f) - (int)baseY;
                  end->valid = 1;
               }
            }
         } else {
            penX += fsize;
         }
         if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
         prev = code;
      }

      // reset slant so the next pass / other render calls aren't left slanted
      if (curItalic) cellFontSetupRenderEffectSlant(&f->font, 0.0f);
   }

   if (reveal) reveal->count = nItems < TEXT_REVEAL_MAX ? nItems : TEXT_REVEAL_MAX;

   // crop the blank rows above the first line's glyphs
   int skip = (int)baseY + 4;
   if (end && end->valid) {
      end->endTop -= skip;    if (end->endTop < 0) end->endTop = 0;
      end->endBottom -= skip; if (end->endBottom < 0) end->endBottom = 0;
      end->lineTop -= skip;   // NOT clamped: the cell top can legitimately be above row 0
      end->lineHeight = (int)(lineH + 0.5f);
      end->inkTop    = (inkMinY <= inkMaxY) ? inkMinY - skip : 0;  if (end->inkTop < 0) end->inkTop = 0;
      end->inkBottom = (inkMinY <= inkMaxY) ? inkMaxY - skip : 0;  if (end->inkBottom < 0) end->inkBottom = 0;
   }
   *outW = maxX + (hasShadow && sdx > 0 ? sdx : 0);
   if (*outW > surfW) *outW = surfW;
   *outH = surfH - skip;
   *outSurfW = surfW;

   free(items);
   free(lineW);

   if (*outW <= 0 || *outH <= 0) { free(buf); return NULL; }

   uint8_t *cropped = buf + skip * surfW * 4;
   memmove(buf, cropped, (*outH) * surfW * 4);

   return buf;
}

void freeTextTexture(TextTexture *tt)
{
   if (tt->valid) {
      retireGfxTexture(&tt->tex);   // given back once the frame that may name it has been shown
      tt->valid = 0;
   }
   tt->slotW = 0;
   tt->slotH = 0;
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode);

void renderFont(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderFontEx(tt, f, size, text, color, maxWidth, wrap, NULL, NULL);
}

void renderFontEx(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end)
{
   renderFontTyped(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL);
}

void renderFontAligned(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextAlign align)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL, align, 0);
}

void renderFontTyped(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, reveal, TEXT_ALIGN_LEFT, 0);
}

void renderFontRaw(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, NULL, NULL, NULL, TEXT_ALIGN_LEFT, 1);
}

// renders ONE glyph at `size` px into its own tightly-cropped white texture (each pixel [A,R,G,B] with
// RGB = 255 and A = coverage). Unlike renderFont this uses no line box and no top crop, so tall glyphs
// (icon fonts) are never clipped - the texture IS the glyph's own bitmap. *offsetX/*offsetY (may be NULL)
// receive the glyph ink's position relative to the em-box top-left, so glyphs of the same size drawn at a
// common anchor line up exactly as the font intends (no scaling/fitting needed). 0 + fills the outputs on
// success, -1 on failure; free *out with freeGfxTexture. Rasterise once at init (never per frame).
int renderGlyphTexture(Font *f, int size, uint32_t code, GfxTexture *out, int *offsetX, int *offsetY)
{
   if (!f->open || size <= 0) return -1;

   cellFontSetScalePixel(&f->font, (float)size, (float)size);
   cellFontBindRenderer(&f->font, &fontRenderer);
   cellFontSetupRenderScalePixel(&f->font, (float)size, (float)size);

   // a scratch surface with room around the glyph so the renderer never scissor-clips it; we read the
   // glyph's own bitmap (transInfo.Image), not the surface, so its exact size only needs to fit here.
   int surfDim = size * 3 + 8;
   uint8_t *surfBuf = (uint8_t *)malloc((size_t)surfDim * surfDim * 4);
   if (!surfBuf) return -1;
   memset(surfBuf, 0, (size_t)surfDim * surfDim * 4);

   CellFontRenderSurface surf;
   cellFontRenderSurfaceInit(&surf, surfBuf, surfDim * 4, 4, surfDim, surfDim);
   cellFontRenderSurfaceSetScissor(&surf, 0, 0, surfDim, surfDim);

   CellFontGlyphMetrics metrics;
   CellFontImageTransInfo transInfo;
   int ret = cellFontRenderCharGlyphImage(&f->font, code, &surf, (float)size, (float)(size * 2), &metrics, &transInfo);
   if (ret != CELL_OK) { free(surfBuf); return -1; }

   int w = transInfo.imageWidth, h = transInfo.imageHeight;
   if (w <= 0 || h <= 0) { free(surfBuf); return -1; }

   uint32_t *argb = (uint32_t *)malloc((size_t)w * h * 4);
   if (!argb) { free(surfBuf); return -1; }
   const uint8_t *coverage = (const uint8_t *)transInfo.Image;
   for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
         argb[y * w + x] = ((uint32_t)coverage[y * transInfo.imageWidthByte + x] << 24) | 0x00FFFFFFu;

   uint32_t offset = uploadGfxTexture(argb, w, h, w * 4);
   free(argb);
   free(surfBuf);
   if (!offset) return -1;

   out->offset = offset;
   out->w = w;
   out->h = h;
   out->pitch = (int)(((uint32_t)(w * 4) + 63u) & ~63u);

   // where the ink sits inside the em box: ink top-left = (bearingX, ascent - bearingY) from the em top.
   int ascentPx = (f->unitsPerEm > 0) ? (int)ceilf((float)f->hheaAscent * (float)size / (float)f->unitsPerEm) : size;
   if (offsetX) *offsetX = (int)(metrics.Horizontal.bearingX + 0.5f);
   if (offsetY) *offsetY = ascentPx - (int)(metrics.Horizontal.bearingY + 0.5f);
   return 0;
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode)
{
   if (end) memset(end, 0, sizeof *end);
   if (reveal) reveal->count = 0;
   if (!f->open) return;

   // clear case: hide the texture but keep the slot for reuse (a permanently
   // empty label is released at teardown via freeTextTexture, not here, so we
   // don't stall the RSX clearing rows during scrolling).
   if (!text || !text[0]) {
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   int drawW, drawH, surfW;
   uint8_t *buf = rasterize(f, size, text, color, maxWidth, wrap, shadow, end, reveal, align, rawMode, &drawW, &drawH, &surfW);
   if (!buf) {
      // nothing rasterized (e.g. all-whitespace after wrapping): keep any
      // existing slot but show nothing.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   // A slot the GPU may still be sampling cannot be overwritten, and waiting for it to finish shows
   // as a stutter and as torn text when a screen changes a lot of labels at once. The new picture
   // goes somewhere else and the old slot is handed back once this frame has been shown.
   freeTextTexture(tt);

   uint32_t offset = uploadGfxTexture(buf, drawW, drawH, surfW * 4);
   free(buf);
   if (offset == 0) {
      // upload failed (out of VRAM): leave the slot empty.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   tt->tex.offset = offset;
   tt->tex.w = drawW;
   tt->tex.h = drawH;
   tt->tex.pitch = (drawW * 4 + 63) & ~63;
   tt->slotW = drawW;
   tt->slotH = drawH;
   tt->valid = 1;
}

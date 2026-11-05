// ui-fonts - the shared Manrope weights, the FONT_POP fallback, and the label helpers that choose between
// them (see ui-fonts.h).

#include "ui-fonts.h"
#include "ui-assets.h"          // the embedded Manrope files
#include "manrope-coverage.h"   // manropeHasCodepoint
#include "dbg.h"                // logInfo / logError
#include <string.h>             // memcpy / memset

static Font manrope[UI_WEIGHT_COUNT];
static Font fallbackFont;       // FONT_POP
static int  manropeOpen;        // how many weights opened
static int  fontsReady;         // initUiFonts ran
static int  fontsClosed;        // termUiFonts ran: never quietly open them again behind the exit path

static const char *FONT_NAMES[UI_WEIGHT_COUNT] = {
   "Manrope Regular", "Manrope SemiBold", "Manrope Bold", "Manrope ExtraBold"
};

static void openManrope(UiWeight weight, const unsigned char *data, unsigned int size)
{
   manrope[weight] = openFontMemory(data, size);   // keeps its own copy of the file (font.c)
   if (manrope[weight].open) manropeOpen++;
   else logError("[ui] %s did not open; its text uses the nearest other weight, or FONT_POP\n", FONT_NAMES[weight]);
}

void initUiFonts(void)
{
   if (fontsReady || fontsClosed) return;
   fontsReady = 1;

   // FONT_POP is a system font set, so it takes none of the 8 user-font slots; the four Manrope weights
   // take four of them and the icon font one more - 5 of 8, and nothing else in the app opens one.
   fallbackFont = openSystemFont(FONT_POP);
   if (!fallbackFont.open) logError("[ui] FONT_POP did not open; text Manrope cannot draw will stay blank\n");

   openManrope(UI_WEIGHT_REGULAR,   manropeRegularData,   manropeRegularDataSize);
   openManrope(UI_WEIGHT_SEMIBOLD,  manropeSemiBoldData,  manropeSemiBoldDataSize);
   openManrope(UI_WEIGHT_BOLD,      manropeBoldData,      manropeBoldDataSize);
   openManrope(UI_WEIGHT_EXTRABOLD, manropeExtraBoldData, manropeExtraBoldDataSize);

   logInfo("[ui] fonts: Manrope %d of %d weights open, FONT_POP %s\n", manropeOpen, (int)UI_WEIGHT_COUNT,
           fallbackFont.open ? "open" : "missing");
}

void termUiFonts(void)
{
   if (!fontsReady) return;
   for (int weight = 0; weight < UI_WEIGHT_COUNT; weight++) closeFont(&manrope[weight]);
   closeFont(&fallbackFont);
   manropeOpen = 0;
   fontsReady  = 0;
   fontsClosed = 1;
}

// a getter called before main.c got to initUiFonts (a module that makes a label early) would otherwise
// hand out a font that was never opened - every label on it blank, and nothing in the log to say why.
static void ensureFonts(void)
{
   if (!fontsReady) initUiFonts();
}

int uiHasManrope(void)
{
   ensureFonts();
   return manropeOpen > 0;
}

Font *uiGetFont(UiWeight weight)
{
   ensureFonts();
   int wanted = ((int)weight >= 0 && weight < UI_WEIGHT_COUNT) ? (int)weight : (int)UI_WEIGHT_REGULAR;
   // A weight that failed to open borrows the nearest one that did - the heavier first at equal distance,
   // so a title never ends up lighter than the text under it - before the whole look drops to FONT_POP.
   for (int distance = 0; distance < UI_WEIGHT_COUNT; distance++) {
      int heavier = wanted + distance, lighter = wanted - distance;
      if (heavier < UI_WEIGHT_COUNT && manrope[heavier].open) return &manrope[heavier];
      if (lighter >= 0 && manrope[lighter].open) return &manrope[lighter];
   }
   return &fallbackFont;
}

Font *uiGetFallbackFont(void)
{
   ensureFonts();
   return &fallbackFont;
}

// One UTF-8 sequence, decoded exactly as decodeUtf8 in the library's font.c does it (static there, so it
// cannot be called from here). Mirroring it matters: the coverage check has to see the very codepoints the
// renderer will draw. Where font.c meets a malformed sequence it draws the lead byte as a character of its
// own; that is reported here as 0, and the text goes to FONT_POP. Reading p[1..3] is safe: a NUL is never
// a continuation byte, so the && chain stops at the terminator.
static int nextCodepoint(const unsigned char **pp, uint32_t *code)
{
   const unsigned char *p = *pp;
   uint32_t c = *p;
   if (c < 0x80) { (*pp)++; *code = c; return 1; }
   if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      *pp += 2;
      *code = ((c & 0x1F) << 6) | (p[1] & 0x3F);
      return 1;
   }
   if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      *pp += 3;
      *code = ((c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      return 1;
   }
   if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      *pp += 4;
      *code = ((c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      return 1;
   }
   (*pp)++;
   *code = c;
   return 0;
}

int uiTextFitsManrope(const char *text)
{
   if (!text) return 1;
   const unsigned char *p = (const unsigned char *)text;
   while (*p) {
      uint32_t code;
      if (!nextCodepoint(&p, &code)) return 0;
      // a line break or a tab is layout, never a glyph: '\n' is handled before decoding (font.c), and
      // manrope-coverage.h lists neither it nor '\t' - counting them would send every multi-line notice
      // and every description to FONT_POP
      if (code == '\n' || code == '\r' || code == '\t') continue;
      if (!manropeHasCodepoint(code)) return 0;
   }
   return 1;
}

Font *uiPickFont(UiWeight weight, const char *text)
{
   ensureFonts();
   if (manropeOpen > 0 && uiTextFitsManrope(text)) return uiGetFont(weight);
   return &fallbackFont;
}

// The text exactly as a Label will hold it: at most LABEL_MAX_TEXT - 1 bytes, like setLabelText's own
// copy - but cut at a character boundary. setLabelText cuts bytewise, and half a UTF-8 sequence is drawn as
// a stray Latin-1 letter; checking the coverage of the full text while the label shows a cut one could also
// pick the wrong font. `out` is always a separate buffer, so the label's own text may be passed in.
static void copyLabelText(char *out, const char *text)
{
   if (!text) text = "";
   int length = 0;
   while (length < LABEL_MAX_TEXT - 1 && text[length]) length++;
   if (text[length])   // it was cut: back off over a split multi-byte sequence
      while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
   memcpy(out, text, (size_t)length);
   out[length] = '\0';
}

void uiInitLabel(Label *l, UiWeight weight, int x, int y, int width, int size, uint32_t color, TextWrap wrap,
                 const char *text)
{
   initLabel(l, uiGetFont(weight), x, y, width, AUTO, size, color, wrap, text);
}

void uiInitTextLabel(Label *l, UiWeight weight, int x, int y, int width, int size, uint32_t color,
                     TextWrap wrap, const char *text)
{
   char clipped[LABEL_MAX_TEXT];
   copyLabelText(clipped, text);
   initLabelRaw(l, uiPickFont(weight, clipped), x, y, width, AUTO, size, color, wrap, clipped);
}

void uiSetTextLabel(Label *l, UiWeight weight, const char *text)
{
   char clipped[LABEL_MAX_TEXT];
   copyLabelText(clipped, text);   // first: freeLabel below empties l->text, which may be what text points at
   Font *font = uiPickFont(weight, clipped);
   if (font != l->font || !l->raw) {
      // a Label renders with the font it holds and setLabelText skips text it already shows, so a change
      // of font - or a label that was not raw yet - needs the old texture dropped before the new render
      freeLabel(l);
      l->font = font;
      l->raw  = 1;
   }
   setLabelText(l, clipped);
}

void uiSetLabelStyle(Label *l, UiWeight weight, int size, uint32_t color)
{
   Font *font = l->raw ? uiPickFont(weight, l->text) : uiGetFont(weight);
   if (font == l->font && size == l->size && color == l->color) return;

   char text[LABEL_MAX_TEXT];
   memcpy(text, l->text, sizeof text);   // freeLabel empties l->text
   text[LABEL_MAX_TEXT - 1] = '\0';
   freeLabel(l);
   l->font  = font;
   l->size  = size;
   l->color = color;
   setLabelText(l, text);
}

// ---- text metrics ----

#define METRICS_CACHE_MAX 64

typedef struct {
   const Font   *font;
   int           size;
   UiTextMetrics metrics;
} MetricsEntry;

static MetricsEntry metricsCache[METRICS_CACHE_MAX];
static int          metricsCount;
static int          metricsFullLogged;

static const char *describeFont(const Font *font)
{
   if (font == &fallbackFont) return "FONT_POP";
   for (int weight = 0; weight < UI_WEIGHT_COUNT; weight++)
      if (font == &manrope[weight]) return FONT_NAMES[weight];
   return "a screen's own font";
}

// what to use until a real measurement exists: Manrope's proportions (a one-line texture is the size plus
// 30 %, capitals 72 % of the size, the texture starting about 4 px into the line box)
static UiTextMetrics estimateMetrics(int size)
{
   UiTextMetrics m;
   m.lineH    = size + (size * 37 + 99) / 100;
   m.texH     = size + (size * 3 + 9) / 10;
   m.baseline = size + (size * 7) / 100 - 4;
   m.capTop   = m.baseline - (size * 72 + 50) / 100;
   if (m.capTop < 0) m.capTop = 0;
   return m;
}

// Renders "H" once and reads the ink box back: its top is the cap height, its bottom the baseline. That is
// the only way to know where FONT_POP puts them (its layout comes from cellFont, not from the font file),
// and it is exact for Manrope too rather than a formula with the crop guessed. 0 when nothing rendered -
// before initGfx, or with a font that is not open.
static int measureMetrics(Font *font, int size, UiTextMetrics *out)
{
   TextTexture probe;
   TextEnd end;
   memset(&probe, 0, sizeof probe);
   // a surface two em wide holds the "H" with room to spare; AUTO would clear a 1024-px-wide one for it.
   // The width changes nothing about the rows, which is all this is after.
   renderFontEx(&probe, font, size, "H", 0xFFFFFFFF, size * 2 + 8, TEXT_NOWRAP, NULL, &end);
   int ok = end.valid && probe.tex.h > 0 && end.inkBottom > end.inkTop;
   if (ok) {
      out->texH     = probe.tex.h;
      out->capTop   = end.inkTop;
      out->baseline = end.inkBottom;
      out->lineH    = end.lineHeight > 0 ? end.lineHeight : probe.tex.h;
   }
   freeTextTexture(&probe);   // retired, so it is safe mid-frame
   return ok;
}

UiTextMetrics uiGetTextMetrics(Font *font, int size)
{
   if (!font || size <= 0) return estimateMetrics(size > 0 ? size : 1);
   for (int i = 0; i < metricsCount; i++)
      if (metricsCache[i].font == font && metricsCache[i].size == size) return metricsCache[i].metrics;

   UiTextMetrics m;
   if (!measureMetrics(font, size, &m)) return estimateMetrics(size);   // not cached: try again next time

   if (metricsCount < METRICS_CACHE_MAX) {
      metricsCache[metricsCount].font    = font;
      metricsCache[metricsCount].size    = size;
      metricsCache[metricsCount].metrics = m;
      metricsCount++;
      // one line per (font, size): on the first hardware run these tell whether text sits where the
      // layout expects it, without a debugger
      logInfo("[ui] text %s %dpx: texture %d rows, capitals %d..%d, line %d\n",
              describeFont(font), size, m.texH, m.capTop, m.baseline, m.lineH);
   } else if (!metricsFullLogged) {
      metricsFullLogged = 1;
      logWarn("[ui] text-metrics cache full (%d); further sizes are measured on every call\n", METRICS_CACHE_MAX);
   }
   return m;
}

int uiTextYForBaseline(Font *font, int size, int baselineY)
{
   UiTextMetrics m = uiGetTextMetrics(font, size);
   return baselineY - m.baseline;
}

int uiTextYForCenter(Font *font, int size, int boxY, int boxH)
{
   UiTextMetrics m = uiGetTextMetrics(font, size);
   // the middle of the capitals, (capTop + baseline) / 2, onto the middle of the box
   return boxY + (boxH - m.capTop - m.baseline) / 2;
}

int uiLabelYForBaseline(const Label *l, int baselineY) { return uiTextYForBaseline(l->font, l->size, baselineY); }
int uiLabelYForCenter(const Label *l, int boxY, int boxH) { return uiTextYForCenter(l->font, l->size, boxY, boxH); }

void uiPlaceLabelBaseline(Label *l, int x, int baselineY) { moveLabel(l, x, uiLabelYForBaseline(l, baselineY)); }
void uiPlaceLabelCentered(Label *l, int x, int boxY, int boxH) { moveLabel(l, x, uiLabelYForCenter(l, boxY, boxH)); }

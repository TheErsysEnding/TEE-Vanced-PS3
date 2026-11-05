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

// ---- room for descenders ----
//
// The library cuts a text texture to the hhea line box: it drops ascender + 4 rows above the first line and
// keeps size + |descender| rows (font.c, rasterize). cellFont, though, puts the baseline as far below the pen
// as the font's bounding box reaches up (head.yMax - Manrope's stacked Vietnamese capitals, 1.12 to 1.23 em
// with the weight), not as far as the hhea ascender (1.066 em). So every line sits lower in its texture than
// the library allows for, and what falls off the bottom are the descenders: at 46/800 the texture ended 6 px
// under the baseline, half way down a "g" (0.255 em), and the comma accents under s and t reach 0.315 em.
//
// Moving the line box up by `shift` units - ascender - shift, descender - shift - keeps the line pitch
// (ascender - descender, give or take a pixel of rounding) and keeps the glyphs where they were under the
// texture's top (the pen and the crop move together), and it adds shift * size / upm rows at the bottom.
// With shift = yMax - yMin - upm + descender the texture runs on below the pen for the whole height of the
// bounding box past the 4 cropped rows: the deepest glyph, its antialiased edge and a row to spare, at every
// size. The Font structs are this module's; font.c reads the two fields only to lay out lines of text, and
// in renderGlyphTexture, which only the icon font goes through.

static int readS16(const unsigned char *p) { return (int)(int16_t)(uint16_t)((p[0] << 8) | p[1]); }

// head.yMin and head.yMax - the font's bounding box in font units - from the SFNT data. 0 if not found.
static int readFontBounds(const unsigned char *data, unsigned int size, int *yMin, int *yMax)
{
   if (!data || size < 12u) return 0;
   unsigned int tables = ((unsigned int)data[4] << 8) | data[5];
   for (unsigned int i = 0; i < tables; i++) {
      unsigned int record = 12u + i * 16u;   // tag, checksum, offset, length
      if (record + 16u > size) return 0;
      const unsigned char *r = data + record;
      if (memcmp(r, "head", 4) != 0) continue;
      unsigned int offset = ((unsigned int)r[8] << 24) | ((unsigned int)r[9] << 16) | ((unsigned int)r[10] << 8) | r[11];
      if (offset > size || size - offset < 54u) return 0;   // the head table is 54 bytes
      *yMin = readS16(data + offset + 38);
      *yMax = readS16(data + offset + 42);
      return 1;
   }
   return 0;
}

static void makeRoomForDescenders(UiWeight weight, const unsigned char *data, unsigned int size)
{
   Font *font = &manrope[weight];
   int yMin, yMax;
   if (font->unitsPerEm <= 0 || !readFontBounds(data, size, &yMin, &yMax)) {
      logWarn("[ui] %s: no line metrics to correct, so its descenders may be cut off\n", FONT_NAMES[weight]);
      return;
   }
   int shift = yMax - yMin - font->unitsPerEm + font->hheaDescent;   // hheaDescent is negative
   // the line box may not start above the pen: the crop would then begin above the start of the buffer
   if (shift > font->hheaAscent) shift = font->hheaAscent;
   if (shift <= 0) return;   // this font's texture already holds its deepest glyph
   logInfo("[ui] %s: line box %d of %d units higher for its descenders (box %d..%d, hhea %d..%d)\n",
           FONT_NAMES[weight], shift, font->unitsPerEm, yMin, yMax, font->hheaDescent, font->hheaAscent);
   font->hheaAscent  -= shift;
   font->hheaDescent -= shift;
}

static void openManrope(UiWeight weight, const unsigned char *data, unsigned int size)
{
   manrope[weight] = openFontMemory(data, size);   // keeps its own copy of the file (font.c)
   if (manrope[weight].open) { manropeOpen++; makeRoomForDescenders(weight, data, size); }
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

// ---- text the fonts cannot draw ----
//
// Neither font has a single emoji: the console's fonts map nothing past U+FFFF, and Manrope ends at U+FB02.
// Over a character that no font has, the renderer advances one em of blank (font.c, rasterize), so every
// emoji left a hole two or three letters wide in a title, a flag one twice as wide, and a title that began
// or broke its line at one started with a blank indent. One emoji was also enough to send the whole title to
// FONT_POP, a lighter face than the Manrope titles beside it. uiCleanText leaves such characters out before
// the font is chosen.

// The symbols FONT_POP can draw inside SYMBOL_BLOCKS. FONT_POP is CELL_FONT_TYPE_DEFAULT_GOTHIC_LATIN_SET:
// Rodin for Latin, NewRodin for the rest. This is the union of the cmaps of SCE-PS3-RD-R-LATIN.TTF and
// SCE-PS3-NR-R-JPN.TTF in dev_flash/data/font - the same files on every firmware and in RPCS3. Both cmaps
// stop at U+FFFF, which is why nothing above it counts as drawable here.
static const uint32_t FONT_POP_SYMBOLS[][2] = {
   {0x2010, 0x2016}, {0x2018, 0x201A}, {0x201C, 0x201E}, {0x2020, 0x2022}, {0x2025, 0x2026}, {0x2030, 0x2030},
   {0x2032, 0x2033}, {0x2039, 0x203C}, {0x203E, 0x203E}, {0x2044, 0x2044}, {0x2049, 0x2049}, {0x2070, 0x2070},
   {0x2074, 0x2079}, {0x2080, 0x2089}, {0x20DD, 0x20DD}, {0x2100, 0x2100}, {0x2103, 0x2103}, {0x2105, 0x2105},
   {0x2109, 0x210A}, {0x210F, 0x210F}, {0x2113, 0x2113}, {0x2116, 0x2117}, {0x2121, 0x2122}, {0x2126, 0x2126},
   {0x212B, 0x212B}, {0x2135, 0x2135}, {0x2153, 0x215E}, {0x2160, 0x216B}, {0x2170, 0x217B}, {0x217F, 0x217F},
   {0x2190, 0x2199}, {0x21C4, 0x21C6}, {0x21CC, 0x21CC}, {0x21D0, 0x21D0}, {0x21D2, 0x21D2}, {0x21D4, 0x21D4},
   {0x21E6, 0x21E9}, {0x2200, 0x2200}, {0x2202, 0x2203}, {0x2205, 0x2208}, {0x220A, 0x220B}, {0x2211, 0x2213},
   {0x2219, 0x221A}, {0x221D, 0x2220}, {0x2225, 0x2225}, {0x2227, 0x222E}, {0x2234, 0x2235}, {0x223C, 0x223D},
   {0x2243, 0x2243}, {0x2248, 0x2248}, {0x2252, 0x2252}, {0x2260, 0x2261}, {0x2264, 0x2267}, {0x226A, 0x226B},
   {0x2272, 0x2273}, {0x2282, 0x2283}, {0x2286, 0x2287}, {0x2295, 0x2298}, {0x229D, 0x229E}, {0x22A0, 0x22A0},
   {0x22A5, 0x22A5}, {0x22BF, 0x22BF}, {0x22EE, 0x22EF}, {0x2300, 0x2300}, {0x2307, 0x2307}, {0x2312, 0x2312},
   {0x244A, 0x244A}, {0x2460, 0x2490}, {0x249C, 0x24EA}, {0x2500, 0x254B}, {0x2550, 0x2550}, {0x255E, 0x255E},
   {0x2561, 0x2561}, {0x256A, 0x256A}, {0x256D, 0x2573}, {0x2581, 0x258F}, {0x2594, 0x2595}, {0x25A0, 0x25A2},
   {0x25AA, 0x25AB}, {0x25B2, 0x25B3}, {0x25B6, 0x25B7}, {0x25BC, 0x25BD}, {0x25C0, 0x25C1}, {0x25C6, 0x25C7},
   {0x25C9, 0x25C9}, {0x25CB, 0x25CC}, {0x25CE, 0x25CF}, {0x25E2, 0x25E6}, {0x25EF, 0x25EF}, {0x2600, 0x2603},
   {0x2605, 0x2606}, {0x260E, 0x260E}, {0x2612, 0x2612}, {0x261C, 0x261F}, {0x2640, 0x2640}, {0x2642, 0x2642},
   {0x2660, 0x266F}, {0x2702, 0x2702}, {0x271A, 0x271A}, {0x2756, 0x2756}, {0x2776, 0x277E}, {0x27A1, 0x27A1},
   {0x3000, 0x3017}, {0x301C, 0x301D}, {0x301F, 0x3020}, {0x3030, 0x3030}, {0x3033, 0x3036}, {0x3220, 0x3243},
   {0x3280, 0x32B0}, {0x32D0, 0x32FE},
};

// Blocks with symbols and punctuation only, no letters: a character in them that neither font draws can go
// without a word going missing. FONT_POP_SYMBOLS covers exactly these.
static const uint32_t SYMBOL_BLOCKS[][2] = {
   {0x2010, 0x209F},   // general punctuation from the hyphens on, super- and subscripts
   {0x20D0, 0x2BFF},   // marks for symbols, letterlike symbols, arrows, maths, technical, enclosed numbers,
                       // box drawing, shapes, miscellaneous symbols, dingbats, braille
   {0x2E00, 0x2E7F},   // supplemental punctuation
   {0x3000, 0x303F},   // CJK symbols and punctuation
   {0x3200, 0x32FF},   // enclosed CJK letters
   {0xFFF0, 0xFFFF},   // specials: the replacement character
};

// Decoration - arrows, shapes, stars, notes, hearts, check marks, most of them emoji as well. Decoration never
// decides the font: a Latin title with a star in it stays in Manrope and loses the star (Manrope has none),
// a Japanese one, in FONT_POP anyway, keeps it. Letters, digits and real punctuation (primes, circled
// numbers, CJK brackets) still send a text to FONT_POP when only FONT_POP has them, as before.
static const uint32_t ORNAMENT_BLOCKS[][2] = {
   {0x203C, 0x203C}, {0x2049, 0x2049},   // the emoji exclamation marks
   {0x20D0, 0x20FF},                     // enclosing marks, the keycap among them
   {0x2139, 0x2139},                     // information source
   {0x2190, 0x21FF},                     // arrows
   {0x2300, 0x23FF},                     // watches, hourglasses, media controls
   {0x24C2, 0x24C2},                     // circled M
   {0x2500, 0x27BF},                     // box drawing, blocks, shapes, miscellaneous symbols, dingbats
   {0x27F0, 0x297F},                     // arrows, braille (its blank is used as an invisible space)
   {0x2B00, 0x2BFF},                     // more arrows, squares, stars and circles
   {0x3030, 0x3030}, {0x303D, 0x303D},   // wavy dash, part alternation mark
   {0x3297, 0x3299},                     // circled ideographs
};

static int inRanges(const uint32_t (*ranges)[2], int count, uint32_t c)
{
   int lo = 0, hi = count - 1;
   while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (c < ranges[mid][0]) hi = mid - 1;
      else if (c > ranges[mid][1]) lo = mid + 1;
      else return 1;
   }
   return 0;
}
#define IN_RANGES(table, c) inRanges(table, (int)(sizeof (table) / sizeof (table)[0]), (c))

// A "fancy" letter as the plain one it stands for: the bold, italic, script, fraktur, double-struck,
// sans-serif and monospace alphabets of the maths symbols, and the boxed and circled capitals, which text
// generators use to style titles and channel names. No font of the console has them, and they are words, not
// decoration - left out, a title set in bold maths letters would vanish. Also the letterlike symbols that
// fill the gaps in those alphabets, and Ohm, Kelvin and Angstrom, which are letters under another code.
// 0 when c is none of these.
static uint32_t plainLetter(uint32_t c)
{
   static const uint32_t LETTERLIKE[][2] = {
      {0x2102, 'C'}, {0x210A, 'g'}, {0x210B, 'H'}, {0x210C, 'H'}, {0x210D, 'H'}, {0x210E, 'h'}, {0x2110, 'I'},
      {0x2111, 'I'}, {0x2112, 'L'}, {0x2115, 'N'}, {0x2119, 'P'}, {0x211A, 'Q'}, {0x211B, 'R'}, {0x211C, 'R'},
      {0x211D, 'R'}, {0x2124, 'Z'}, {0x2126, 0x03A9}, {0x2128, 'Z'}, {0x212A, 'K'}, {0x212B, 0x00C5},
      {0x212C, 'B'}, {0x212D, 'C'}, {0x212F, 'e'}, {0x2130, 'E'}, {0x2131, 'F'}, {0x2133, 'M'}, {0x2134, 'o'},
      {0x2145, 'D'}, {0x2146, 'd'}, {0x2147, 'e'}, {0x2148, 'i'}, {0x2149, 'j'},
   };
   if (c >= 0x2102 && c <= 0x2149) {
      for (int i = 0; i < (int)(sizeof LETTERLIKE / sizeof LETTERLIKE[0]); i++)
         if (LETTERLIKE[i][0] == c) return LETTERLIKE[i][1];
      return 0;
   }
   if (c < 0x1D400) return 0;
   if (c <= 0x1D6A3) {   // 13 alphabets of 52 letters, A-Z then a-z
      uint32_t i = (c - 0x1D400) % 52;
      return i < 26 ? 'A' + i : 'a' + (i - 26);
   }
   if (c == 0x1D6A4) return 'i';   // dotless italic i and j
   if (c == 0x1D6A5) return 'j';
   if (c >= 0x1D7CE && c <= 0x1D7FF) return '0' + (c - 0x1D7CE) % 10;   // 5 sets of digits
   // parenthesized, squared, negative circled and negative squared capitals: 26 letters every 32 codes
   if (c >= 0x1F110 && c <= 0x1F189 && (c - 0x1F110) % 32 < 26) return 'A' + (c - 0x1F110) % 32;
   return 0;
}

// what uiCleanText does with one character of external text
typedef enum {
   CHAR_TEXT,        // kept, and it has a say in the font: letters, digits, punctuation outside the symbol blocks
   CHAR_SYMBOL,      // kept where the text's font draws it, else left out. One that only FONT_POP draws sends
                     // the text to FONT_POP, like a letter would
   CHAR_ORNAMENT,    // decoration: like CHAR_SYMBOL, but never the reason a text goes to FONT_POP
   CHAR_DROP,        // no font of the app draws it: left out, and the spaces around it close up
   CHAR_INVISIBLE    // drawn as nothing anyway (zero-width, direction marks, selectors, controls): left out
                     // without a trace - two words it stands between stay one
} CharKind;

// Sorts one decoded character, and rewrites *code where a plainer character looks the same: the many widths
// of space as ' ' (neither font has them), a tab as ' ', a fancy letter as the plain one.
static CharKind classifyChar(uint32_t *code)
{
   uint32_t c = *code;
   if (c < 0x2000) {
      if (c == '\n') return CHAR_TEXT;
      if (c == '\t') { *code = ' '; return CHAR_TEXT; }
      // control characters, '\r' among them: Manrope has an empty glyph for '\r', FONT_POP a one-em gap
      return (c < 0x20 || (c >= 0x7F && c < 0xA0)) ? CHAR_INVISIBLE : CHAR_TEXT;
   }
   if (c <= 0x200A || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F) {
      *code = ' ';
      return CHAR_TEXT;
   }
   // zero-width spaces and joiners, direction marks, the variation selectors (U+FE0F asks for the emoji
   // look of the character before it), the byte-order mark
   if (c <= 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
       (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF)
      return CHAR_INVISIBLE;
   uint32_t plain = plainLetter(c);
   if (plain) { *code = plain; return CHAR_TEXT; }
   if (c >= 0x10000) {
      // pictographs and emoji, flags, skin tones, tags, musical and maths symbols: no glyph in either font
      if ((c >= 0x1D000 && c <= 0x1D7FF) || (c >= 0x1F000 && c <= 0x1FBFF) || (c >= 0xE0000 && c <= 0xE01EF))
         return CHAR_DROP;
      return CHAR_TEXT;   // letters of scripts neither font has: a gap, but leaving them out would lose words
   }
   if (!IN_RANGES(SYMBOL_BLOCKS, c)) return CHAR_TEXT;
   if (!manropeHasCodepoint(c) && !IN_RANGES(FONT_POP_SYMBOLS, c)) return CHAR_DROP;
   return IN_RANGES(ORNAMENT_BLOCKS, c) ? CHAR_ORNAMENT : CHAR_SYMBOL;
}

// The next character of external text, decoded as font.c will decode it (nextCodepoint), sorted and maybe
// rewritten. *joined carries the emoji joiner U+200D from one call to the next: in a sequence such as "woman
// running" (runner, joiner, female sign) a symbol after the joiner is part of the picture and goes with it,
// even where FONT_POP has it. A malformed byte stays as it is (*malformed): font.c draws it as a Latin-1
// letter, and as before it puts the text in FONT_POP.
static CharKind nextTextChar(const unsigned char **p, uint32_t *code, int *joined, int *malformed)
{
   *malformed = !nextCodepoint(p, code);
   if (*malformed) { *joined = 0; return CHAR_TEXT; }
   uint32_t original = *code;
   CharKind kind = classifyChar(code);
   if (*joined && kind != CHAR_TEXT && kind != CHAR_INVISIBLE) kind = CHAR_DROP;
   *joined = original == 0x200D;
   return kind;
}

// 1 when the text is for FONT_POP: something in it that has a say in the font (not decoration, not left out)
// is not in Manrope.
static int textNeedsFallback(const char *text)
{
   if (fontsReady && manropeOpen == 0) return 1;   // no Manrope at all: every label is FONT_POP
   const unsigned char *p = (const unsigned char *)text;
   int joined = 0;
   while (*p) {
      uint32_t code;
      int malformed;
      CharKind kind = nextTextChar(&p, &code, &joined, &malformed);
      if (malformed) return 1;
      if (kind == CHAR_DROP || kind == CHAR_INVISIBLE || kind == CHAR_ORNAMENT || code == '\n') continue;
      if (!manropeHasCodepoint(code)) return 1;
   }
   return 0;
}

// letters and digits of the alphabetic scripts: a character left out between two of them was keeping two
// words apart ("NEW", fire, "VIDEO"), and a space takes its place
static int isWordChar(uint32_t c)
{
   if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
   return c >= 0xC0 && c < 0x2000 && c != 0xD7 && c != 0xF7;
}

static int trimSpaces(const char *out, int length)
{
   while (length > 0 && out[length - 1] == ' ') length--;
   return length;
}

static int encodeUtf8(uint32_t c, char *out)
{
   if (c < 0x80) { out[0] = (char)c; return 1; }
   if (c < 0x800) {
      out[0] = (char)(0xC0 | (c >> 6));
      out[1] = (char)(0x80 | (c & 0x3F));
      return 2;
   }
   if (c < 0x10000) {
      out[0] = (char)(0xE0 | (c >> 12));
      out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[2] = (char)(0x80 | (c & 0x3F));
      return 3;
   }
   out[0] = (char)(0xF0 | (c >> 18));
   out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
   out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
   out[3] = (char)(0x80 | (c & 0x3F));
   return 4;
}

int uiCleanText(char *out, int outSize, const char *text)
{
   if (!out || outSize <= 0) return 0;
   int capacity = outSize - 1, length = 0;
   if (text) {
      // The font first, over the whole text, so that which symbols stay can depend on it. The output never
      // gets ahead of the input (a character is replaced by one no longer than itself, and the one space put
      // in for a dropped character takes the place it left), so out may be text itself.
      int fallback = textNeedsFallback(text);
      const unsigned char *p = (const unsigned char *)text;
      int joined = 0, dropped = 0;
      uint32_t last = 0;   // the last character written, 0 before the first
      while (*p) {
         uint32_t code;
         int malformed;
         CharKind kind = nextTextChar(&p, &code, &joined, &malformed);
         if ((kind == CHAR_SYMBOL || kind == CHAR_ORNAMENT) &&
             !(fallback ? IN_RANGES(FONT_POP_SYMBOLS, code) : manropeHasCodepoint(code)))
            kind = CHAR_DROP;
         if (kind == CHAR_INVISIBLE) continue;
         if (kind == CHAR_DROP) { dropped = 1; continue; }

         char bytes[5];
         int size = 0;
         if (dropped) {
            // where a character went, the spaces either side of it must not double up, nor start or end a
            // line - "radio [headphones] beats" is "radio beats", not "radio  beats"
            if (code == ' ' && (last == 0 || last == ' ' || last == '\n')) continue;
            if (code == '\n') length = trimSpaces(out, length);
            else if (isWordChar(last) && isWordChar(code)) bytes[size++] = ' ';
         }
         if (malformed) bytes[size++] = (char)code;
         else size += encodeUtf8(code, bytes + size);
         if (length + size > capacity) break;   // full: it ends at a character boundary
         memcpy(out + length, bytes, (size_t)size);
         length += size;
         last = code;
         if (code != ' ' && code != '\n') dropped = 0;
      }
      if (dropped) length = trimSpaces(out, length);
   }
   out[length] = '\0';
   return length;
}

// ---- text that would come out blank ----
//
// What uiCleanText keeps but no font of the console draws - the letters of Thai, Arabic, Hebrew, Georgian, the
// scripts of India and others, which neither Manrope nor any font of FONT_POP's set has (checked against the
// cmaps in dev_flash/data/font) - is drawn as an em of blank each (font.c, rasterize). A word of it inside a
// Latin or Japanese title is a gap the reader can see past; a title made of it is an empty line with a stray
// "|" or digit and a "..." at the end.

// 1 when FONT_POP has a glyph for code. libfont answers CELL_FONT_ERROR_NO_SUPPORT_GLYPH where no font of the
// set has one - exactly where the renderer leaves its blank, since its render call fails the same way - so
// this finds what the renderer will find, including however much Korean and Chinese the set really covers.
// The lookup works at the font's current scale; the callers set one.
static int fallbackHasGlyph(uint32_t code)
{
   CellFontGlyphMetrics metrics;
   return cellFontGetCharGlyphMetrics(&fallbackFont.font, code, &metrics) == CELL_OK;
}

// Whether fallbackHasGlyph can be believed, settled once: FONT_POP must report a capital H. If it does not,
// no text is ever judged illegible, and every text is drawn as it was before. The log line also tells, on
// the first run on a console, which of these scripts its font set lacks.
static int fallbackGlyphsKnown(void)
{
   static int known = -1;   // -1 not asked yet
   if (known < 0 && fallbackFont.open) {
      cellFontSetScalePixel(&fallbackFont.font, (float)UI_MIN_TEXT_SIZE, (float)UI_MIN_TEXT_SIZE);
      known = fallbackHasGlyph('H');
      logInfo("[ui] FONT_POP glyph lookup: H %d, Thai %d, Hangul %d, Devanagari %d%s\n", known,
              fallbackHasGlyph(0x0E01), fallbackHasGlyph(0xAC00), fallbackHasGlyph(0x0915),
              known ? "" : " - it fails, so no text counts as illegible");
   }
   return known > 0;
}

// Letters: what a title is read by. Digits, punctuation, symbols and marks are not - a Thai title keeps its
// "|" and its digits, and those alone do not make it readable.
static int isLetter(uint32_t c)
{
   if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
   if (c < 0xC0 || c == 0xD7 || c == 0xF7) return 0;   // Latin-1 punctuation and signs
   if (c >= 0x02B0 && c < 0x0370) return 0;            // modifier letters, combining accents
   if (c >= 0x2000 && c < 0x2C00) return 0;            // punctuation, numbers, symbols, arrows, shapes ...
   if (c >= 0x2E00 && c < 0x2E80) return 0;            // supplemental punctuation
   if (c >= 0x2FF0 && c < 0x3040) return 0;            // CJK symbols and punctuation
   if (c >= 0x3200 && c < 0x3400) return 0;            // enclosed CJK, CJK compatibility (units, eras)
   if (c >= 0xD800 && c < 0xF900) return 0;            // surrogates, private use
   if (c >= 0xFE00 && c < 0xFE70) return 0;            // variation selectors, vertical and small forms
   if (c >= 0xFF00 && c < 0xFFF0)                      // full- and halfwidth forms: only their letters and kana
      return (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A) || (c >= 0xFF66 && c <= 0xFFDC);
   return c < 0xFFF0 || c >= 0x10000;
}

int uiTextIsLegible(const char *text)
{
   if (!text || !text[0]) return 1;
   ensureFonts();
   if (manropeOpen > 0 && uiTextFitsManrope(text)) return 1;   // drawn in Manrope, which has every character
   if (!fallbackFont.open || !fallbackGlyphsKnown()) return 1;
   cellFontSetScalePixel(&fallbackFont.font, (float)UI_MIN_TEXT_SIZE, (float)UI_MIN_TEXT_SIZE);
   int drawn = 0, blank = 0;
   const unsigned char *p = (const unsigned char *)text;
   while (*p) {
      uint32_t code;
      nextCodepoint(&p, &code);   // a malformed byte stands for itself, as font.c draws it
      if (code == ' ' || code == '\n') continue;
      if (code >= 0x80 && !fallbackHasGlyph(code)) blank++;   // FONT_POP has every printable ASCII character
      else if (isLetter(code)) drawn++;
   }
   // Kept while it draws at least as many letters as it leaves blanks: a word of another script in a Latin
   // title is then a gap the reader sees past ("[gap] | Thailand vs Japan"). With fewer, the blanks are most
   // of the line - and each is an em wide, twice a Latin letter.
   return drawn >= blank;
}

// The text exactly as a Label will hold it: cleaned (uiCleanText) and at most LABEL_MAX_TEXT - 1 bytes, like
// setLabelText's own copy - but cut at a character boundary. setLabelText cuts bytewise, and half a UTF-8
// sequence is drawn as a stray Latin-1 letter; checking the coverage of the full text while the label shows a
// cut one could also pick the wrong font. `out` is always a separate buffer, so the label's own text may be
// passed in.
static void copyLabelText(char *out, const char *text)
{
   uiCleanText(out, LABEL_MAX_TEXT, text);
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
// about 45 % with the room for descenders - see makeRoomForDescenders - capitals 72 % of the size, the
// texture starting about 4 px into the line box)
static UiTextMetrics estimateMetrics(int size)
{
   UiTextMetrics m;
   m.lineH    = size + (size * 37 + 99) / 100;
   m.texH     = size + (size * 45 + 99) / 100;
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

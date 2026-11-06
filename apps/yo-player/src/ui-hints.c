// ui-hints - the Ember button-hint row (see ui-hints.h).

#include "ui-hints.h"
#include "ui-fonts.h"   // uiInitLabel, UI_HINT_SIZE / UI_HINT_WEIGHT, uiLabelYForCenter
#include "theme.h"      // activeTheme->textSecondary
#include "dbg.h"        // logInfo
#include <string.h>     // memset
#include <stdio.h>      // snprintf

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

void uiInitHintsStyled(UiHints *row, int glyphHeight, int captionSize, uint32_t captionColor)
{
   memset(row, 0, sizeof *row);
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

// the glyph scaled to the row's height with its own aspect; a glyph the console did not have keeps its
// square slot, so the captions around it still line up (as the library row does)
static int glyphWidth(const UiHints *row, ConsoleGlyph glyph)
{
   GfxTexture tex = getConsoleGlyph(glyph);
   return tex.h > 0 ? (tex.w * row->glyphHeight + tex.h / 2) / tex.h : row->glyphHeight;
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

      GfxTexture tex = getConsoleGlyph(hint->glyph);
      int width = glyphWidth(row, hint->glyph);
      if (tex.offset)
         drawGfxTexture(x, y, width, row->glyphHeight, tex, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, GFX_FILTER_LINEAR);
      x += width;

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

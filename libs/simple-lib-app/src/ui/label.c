// label - mutable text rendered to texture
#include "ui/label.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>

static void initLabelCommon(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color,
                             TextWrap wrap, int raw, const char *text)
{
   memset(l, 0, sizeof(*l));
   l->font = font;
   l->x = x;
   l->y = y;
   l->width = width;
   l->height = height;
   l->size = size;
   l->color = color;
   l->wrap = wrap;
   l->raw = raw;
   l->text[0] = '\0';
   if (text) setLabelText(l, text);
}

void initLabel(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text)
{
   initLabelCommon(l, font, x, y, width, height, size, color, wrap, 0, text);
}

void initLabelRaw(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text)
{
   initLabelCommon(l, font, x, y, width, height, size, color, wrap, 1, text);
}

// re-rasterises the current text/colour into the texture (raw = no {i}/{b}/{color=} markup parsing).
static void renderLabel(Label *l)
{
   if (l->raw) renderFontRaw(&l->tt, l->font, l->size, l->text, l->color, l->width, l->wrap);
   else        renderFont(&l->tt, l->font, l->size, l->text, l->color, l->width, l->wrap);
}

void setLabelText(Label *l, const char *text)
{
   if (strncmp(l->text, text, LABEL_MAX_TEXT) == 0) return;
   strCopy(l->text, LABEL_MAX_TEXT, text);
   // (TEE Vanced V3.1.0) a text cut to the label's 255 bytes ends at a character boundary: half a UTF-8 sequence would be
   // drawn as a stray letter (a Russian or Arabic text takes two bytes a letter, a Japanese one three)
   int length = (int)strlen(l->text);
   if (length == LABEL_MAX_TEXT - 1 && text[length]) {
      int start = length;
      while (start > 0 && ((unsigned char)l->text[start - 1] & 0xC0) == 0x80) start--;   // back over its continuation bytes
      if (start > 0 && ((unsigned char)l->text[start - 1] & 0xC0) == 0xC0) {           // the lead byte of the last one
         unsigned char lead = (unsigned char)l->text[start - 1];
         int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
         if (length - (start - 1) < need) l->text[start - 1] = 0;                        // cut short: drop it whole
      }
   }
   renderLabel(l);
}

void setLabelColor(Label *l, uint32_t color)
{
   if (l->color == color) return;
   l->color = color;
   if (l->text[0] == '\0') return;   // nothing rendered yet; the next setLabelText will use the new colour
   renderLabel(l);
}

void freeLabel(Label *l)
{
   freeTextTexture(&l->tt);
   l->text[0] = '\0';  // force a re-render if the label is reused
}

void moveLabel(Label *l, int x, int y)
{
   l->x = x;
   l->y = y;
}

void drawLabelAlpha(const Label *l, int alpha)
{
   if (l->tt.tex.w > 0) {
      uint32_t tint = ((uint32_t)(alpha & 0xFF) << 24) | 0x00FFFFFF;
      drawGfxTexture(l->x, l->y, l->tt.tex.w, l->tt.tex.h, l->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_NEAREST);
   }
}

void drawLabel(const Label *l) { drawLabelAlpha(l, (int)(l->color >> 24)); }

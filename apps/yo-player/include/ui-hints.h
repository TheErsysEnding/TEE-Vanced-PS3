#pragma once

// ui-hints - the button-hint row in the Ember look: "[glyph] caption" for each button a screen offers,
// with the console's own PS3 button art (console-glyphs.h, drawn untinted - it is full-colour art and a
// tint could only spoil it) and Manrope captions, 20/600 in textSecondary.
//
// It takes the place of the library's ButtonHints in the app because that row can only be centred or start
// at an x - not end at one, as the Ember rows do at the right edge - and keeps its spacing private (8 px
// from glyph to caption where the design has 10). The model is the same: build the row once in the screen's
// init, retitle or hide hints by index as the state changes, draw it every frame, term it in term.
//
//    static UiHints hints;
//    uiInitHints(&hints);
//    uiAddHint(&hints, GLYPH_CROSS, "Play");             // index 0
//    uiAddHint(&hints, GLYPH_L1, "");                    // an empty caption pairs a glyph with the next:
//    uiAddHint(&hints, GLYPH_R1, "Category");            //   [L1] [R1] Category
//    ...
//    uiSetHintCaption(&hints, 0, "Open channel");        // retitle by index (when the meaning changes)
//    uiDrawHints(&hints, UI_SAFE_RIGHT, UI_HINTS_BOTTOM_Y, UI_ALIGN_RIGHT);   // in draw
//    uiTermHints(&hints);                                // in term

#include "ui/console-glyphs.h"   // ConsoleGlyph, getConsoleGlyph
#include "ui/label.h"            // Label
#include "ui-draw.h"             // UiAlign, UI_SAFE_*
#include <stdint.h>

#define UI_HINTS_MAX         16   // hints per row (as ButtonHints)
#define UI_HINT_GLYPH_H      34   // glyph height; the width follows the art's own aspect
#define UI_HINT_CAPTION_GAP  10   // glyph -> its caption
#define UI_HINT_GAP          30   // one hint -> the next
#define UI_HINT_PAIR_GAP     8    // a caption-less glyph -> the glyph it pairs with (L1 + R1)

// where the standard rows go: the top of the glyph box (each row is UI_HINT_GLYPH_H tall)
#define UI_HINTS_BOTTOM_Y    (UI_SAFE_BOTTOM - UI_HINT_GLYPH_H)   // 998: the row's bottom on the safe line
#define UI_HINTS_TOP_Y       54                                   // a row at the top right, centred on y 71
                                                                  // like the top-right row of Bild 1

typedef struct {
   ConsoleGlyph glyph;
   Label        caption;   // fixed UI text in Manrope; empty when paired
   int          paired;    // no caption: drawn PAIR_GAP before the next hint, whose caption it shares
   int          hidden;    // left out of the row (and of its width) entirely
} UiHint;

typedef struct {
   UiHint   hints[UI_HINTS_MAX];
   int      count;
   int      glyphHeight;
   int      captionSize;
   uint32_t captionColor;
   int      gap;           // between hints; UI_HINT_GAP unless uiSetHintsGap changed it
} UiHints;

// an empty row in the standard look: glyphs UI_HINT_GLYPH_H, captions UI_HINT_SIZE / UI_HINT_WEIGHT in
// activeTheme->textSecondary, UI_HINT_GAP between hints
void uiInitHints(UiHints *row);
// the same with another glyph height, caption size or caption colour (e.g. over the video, Phase 3); 0 for
// either size means the standard one. Captions under UI_MIN_TEXT_SIZE are set in weight 800.
void uiInitHintsStyled(UiHints *row, int glyphHeight, int captionSize, uint32_t captionColor);

// Appends a hint and returns its index - keep it in a #define if the caption changes later, the way the
// screens keep CROSS_HINT / TRIANGLE_HINT. -1 when the row is full. caption "" or NULL pairs the glyph with
// the hint added next (L1 + R1, L2 + R2).
int  uiAddHint(UiHints *row, ConsoleGlyph glyph, const char *caption);

// Retitles a hint (e.g. "Channel" <-> "Unsubscribe"); the row's width follows. Rendering happens only when
// the text actually changes, so calling it on every state check is fine.
void uiSetHintCaption(UiHints *row, int index, const char *caption);

// Shows or hides a hint - for a button that does nothing in the current state. Hide both glyphs of a pair.
void uiSetHintShown(UiHints *row, int index, int shown);

// another gap between hints (0 = back to UI_HINT_GAP)
void uiSetHintsGap(UiHints *row, int gap);

// the row's width in px over its visible hints, gaps included
int  uiGetHintsWidth(const UiHints *row);

// Draws the visible hints in order along y (the top of the glyphs; captions are centred on them), with x
// as the row's left edge, middle or right edge per `align`. Every frame is fine: nothing renders here.
void uiDrawHints(const UiHints *row, int x, int y, UiAlign align);

// frees the captions (the glyphs belong to console-glyphs)
void uiTermHints(UiHints *row);

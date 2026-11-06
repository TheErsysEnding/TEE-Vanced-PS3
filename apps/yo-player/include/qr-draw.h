#pragma once

// qr-draw - draws a QR code kept as a bit matrix: one unsigned int per row, bit x = column x (LSB = leftmost),
// a set bit = a dark module. The matrices themselves (qr-linktree.h, qr-device.h) are generated once on the build
// machine and checked by decoding them back to their URL - see qr-linktree.h for how.

#include "gfx.h"

#define QR_QUIET_MODULES 4   // the quiet zone the spec requires on every side, in modules

// Draws the code at (x, y) with `scale` pixels per module, quiet zone included - so the drawn size is
// (size + 2 * QR_QUIET_MODULES) * scale on both sides. A scanner needs real contrast, so this paints its own
// white ground and black modules whatever the theme is doing.
//
// Horizontal runs are merged into one rectangle each: a 29x29 code has about 440 dark modules that collapse into
// about 220 runs, so a code costs half the rectangles, for pixel-identical output. Worth keeping honest - the
// renderer silently drops draw calls once a frame's batch fills (gfx.c, MAX_QUADS).
static inline void drawQrMatrix(const unsigned int *rows, int size, int x, int y, int scale)
{
   int span = (size + 2 * QR_QUIET_MODULES) * scale;
   fillGfxRectangle(x, y, span, span, 0xFFFFFFFF);

   int originX = x + QR_QUIET_MODULES * scale, originY = y + QR_QUIET_MODULES * scale;
   for (int row = 0; row < size; row++) {
      unsigned int bits = rows[row];
      int col = 0;
      while (col < size) {
         if (!(bits & (1u << col))) { col++; continue; }
         int run = 0;
         while (col + run < size && (bits & (1u << (col + run)))) run++;
         fillGfxRectangle(originX + col * scale, originY + row * scale, run * scale, scale, 0xFF000000);
         col += run;
      }
   }
}

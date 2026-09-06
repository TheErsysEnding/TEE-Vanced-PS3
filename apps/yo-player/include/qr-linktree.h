#pragma once

// qr-linktree - the channel's link as a QR code, baked in as a bit matrix.
//
// A URL on a television is useless: nobody types one with a controller. A QR code is scanned with the phone
// that is already in the viewer's hand. The matrix is generated once on the build machine (segno, error
// correction level M, version 3 = 29x29) and verified by decoding the rendered image back to the exact URL
// before it was pasted here - so what ships cannot be a QR code that scans to something else.
//
// Kept as a matrix rather than a PNG on purpose: no file to ship in the package, nothing to find at runtime,
// no image decoder involved. 116 bytes of rodata and a handful of rectangles.
//
// To change the link, regenerate BOTH the string and the matrix - they are not derived from each other at
// runtime, and an edited URL with the old matrix would be a silent lie:
//   uv run --with segno --with opencv-python-headless --with numpy python - <<'PY'
//   import segno, numpy as np, cv2
//   URL = 'https://linktr.ee/theersysending'
//   q = segno.make(URL, error='m'); m = [[1 if c else 0 for c in r] for r in q.matrix]; n = len(m)
//   S, Q = 12, 4
//   img = np.ones(((n+2*Q)*S,)*2, np.uint8)*255
//   for y,row in enumerate(m):
//      for x,v in enumerate(row):
//         if v: img[(y+Q)*S:(y+Q+1)*S, (x+Q)*S:(x+Q+1)*S] = 0
//   assert cv2.QRCodeDetector().detectAndDecode(img)[0] == URL, 'round-trip failed'
//   print([sum(v << x for x,v in enumerate(row)) for row in m])
//   PY

#define QR_URL       "linktr.ee/theersysending"   // what is printed under the code, without the scheme
#define QR_QUIET     4                            // quiet-zone modules the spec requires on every side
#define QR_SIZE      29

// one bit per module, bit x = column x, LSB = leftmost. Set bit = dark module.
static const unsigned int QR_ROWS[QR_SIZE] = {
   0x1FDF0B7Fu, 0x10520641u, 0x17546C5Du, 0x1743B25Du,
   0x175EA95Du, 0x10526F41u, 0x1FD5557Fu, 0x0005D000u,
   0x118A52FEu, 0x17DEAF2Bu, 0x07F7345Fu, 0x0BE93020u,
   0x1EA245E7u, 0x1B793A04u, 0x08A1EDC7u, 0x03743311u,
   0x061A2971u, 0x1BDCB839u, 0x01D18D71u, 0x0BC53D9Du,
   0x0DFA9ACDu, 0x1B133700u, 0x015CE97Fu, 0x03192541u,
   0x07F2775Du, 0x1CBD215Du, 0x09FEB95Du, 0x0B3D4341u,
   0x054E1C7Fu,
};

// Draws the code at (x, y) with `scale` pixels per module, quiet zone included - so the drawn size is
// (QR_SIZE + 2 * QR_QUIET) * scale on both sides. A scanner needs real contrast, so this paints its own
// white ground and black modules whatever the theme is doing.
//
// Horizontal runs are merged into one rectangle each: this matrix has 439 dark modules that collapse into
// 218 runs, so the box costs 219 rectangles a frame instead of 440, for pixel-identical output. Worth
// keeping honest - the renderer silently drops draw calls once a frame's batch fills (gfx.c, MAX_QUADS).
static inline void drawQrCode(int x, int y, int scale)
{
   int span = (QR_SIZE + 2 * QR_QUIET) * scale;
   fillGfxRectangle(x, y, span, span, 0xFFFFFFFF);

   int originX = x + QR_QUIET * scale, originY = y + QR_QUIET * scale;
   for (int row = 0; row < QR_SIZE; row++) {
      unsigned int bits = QR_ROWS[row];
      int col = 0;
      while (col < QR_SIZE) {
         if (!(bits & (1u << col))) { col++; continue; }
         int run = 0;
         while (col + run < QR_SIZE && (bits & (1u << (col + run)))) run++;
         fillGfxRectangle(originX + col * scale, originY + row * scale, run * scale, scale, 0xFF000000);
         col += run;
      }
   }
}

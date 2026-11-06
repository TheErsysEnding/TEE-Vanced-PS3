#pragma once

// qr-device - Google's device sign-in page as a QR code, for the account screen: the phone in the viewer's hand
// opens the page from it, and the code shown beside it is typed there. Generated once on the build machine like
// qr-linktree.h's (segno, then decoded back with OpenCV to exactly QR_DEVICE_LINK before it was pasted here):
//   segno.make("https://www.google.com/device", error="m")   -> version 3, 29x29 (segno raised it to level Q)
// A changed link needs a regenerated matrix: they are not derived from each other at run time.

#include "qr-draw.h"

#define QR_DEVICE_LINK  "https://www.google.com/device"   // what the matrix encodes
#define QR_DEVICE_SHOWN "google.com/device"               // what is printed beside it, without the scheme
#define QR_DEVICE_SIZE  29

// one bit per module, bit x = column x, LSB = leftmost. Set bit = dark module.
static const unsigned int QR_DEVICE_ROWS[QR_DEVICE_SIZE] = {
   0x1FDD107Fu, 0x10453941u, 0x174CA75Du, 0x175EFC5Du,
   0x174BC25Du, 0x105B3641u, 0x1FD5557Fu, 0x000F6C00u,
   0x0C0F686Eu, 0x137F2733u, 0x0E550559u, 0x13909230u,
   0x1CB24843u, 0x16CA5E8Du, 0x1BBDA0E8u, 0x0B599631u,
   0x1BA30BCEu, 0x04916BB0u, 0x04D3C749u, 0x0F66511Cu,
   0x0FFF70EEu, 0x1B1D0D00u, 0x0D5B007Fu, 0x0910B541u,
   0x07F47C5Du, 0x0D09355Du, 0x1497A95Du, 0x0B110341u,
   0x089CC07Fu,
};

// (QR_DEVICE_SIZE + 2 * QR_QUIET_MODULES) * scale on both sides, quiet zone included
static inline void drawQrDevice(int x, int y, int scale) { drawQrMatrix(QR_DEVICE_ROWS, QR_DEVICE_SIZE, x, y, scale); }

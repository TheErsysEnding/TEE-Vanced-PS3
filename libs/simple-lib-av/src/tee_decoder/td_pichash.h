/* TEE Decoder building block - made by tools/make_baustein.sh from common/pichash.h; do not edit here: change the
   project's file and run the script again. */
/* TEE Decoder: the per-picture check hash. FNV-1a over the cropped Y, U and V planes read as big-endian
   32-bit words, in 4 lanes (word i of a row goes to lane i & 3), so the PPU keeps 4 multiplies in flight;
   the lanes are folded into one value at the end. Rows must be a multiple of 16 bytes wide (1920 / 960).
   The Mac's reference tool (mac/dref_tool) and the PS3's check thread use this same code. */
#ifndef PICHASH_H
#define PICHASH_H

#include <stdint.h>

#define PICHASH_PRIME 0x01000193u

static inline uint32_t pichash_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void pichash_plane(uint32_t h[4], const uint8_t *p, int stride, int w, int rows)
{
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
	int y, x;

	for (y = 0; y < rows; y++, p += stride)
		for (x = 0; x < w; x += 16) {
#if defined(__PPU__) || defined(__powerpc64__) || defined(__powerpc__)
			const uint32_t *q = (const uint32_t *)(const void *)(p + x);    /* big-endian loads */

			a = (a ^ q[0]) * PICHASH_PRIME;
			b = (b ^ q[1]) * PICHASH_PRIME;
			c = (c ^ q[2]) * PICHASH_PRIME;
			d = (d ^ q[3]) * PICHASH_PRIME;
#else
			a = (a ^ pichash_be32(p + x)) * PICHASH_PRIME;
			b = (b ^ pichash_be32(p + x + 4)) * PICHASH_PRIME;
			c = (c ^ pichash_be32(p + x + 8)) * PICHASH_PRIME;
			d = (d ^ pichash_be32(p + x + 12)) * PICHASH_PRIME;
#endif
		}
	h[0] = a;
	h[1] = b;
	h[2] = c;
	h[3] = d;
}

/* the whole cropped picture: y/u/v point at sample (0, 0) of the crop window */
static inline uint32_t pichash_picture(const uint8_t *y, int sy, const uint8_t *u, const uint8_t *v, int sc, int w, int hgt)
{
	uint32_t h[4] = { 0x811C9DC5u, 0x050C5D1Fu, 0x2D4B0E7Bu, 0x6B8B4567u }, r = 0x811C9DC5u;
	int k;

	pichash_plane(h, y, sy, w, hgt);
	pichash_plane(h, u, sc, w / 2, hgt / 2);
	pichash_plane(h, v, sc, w / 2, hgt / 2);
	for (k = 0; k < 4; k++)
		r = (r ^ h[k]) * PICHASH_PRIME;
	return r;
}

#endif

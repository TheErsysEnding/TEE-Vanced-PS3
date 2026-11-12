#!/usr/bin/env python3
"""Recolour the XMB artwork from the old orange to the ice-blue palette (V3.0.0).

TEE (2026-11-11): black and orange had to go - people took the app for, or joked about it as, an adult site's
look; it was meant as Black Ops 2's. The artwork keeps its layout, wordmark and text: every warm pixel (hue 0 -
70 degrees: the orange, the light orange, the cream text, the brown grounds) gets the accent's hue, sky blue
#38BDF8, with its brightness kept and its saturation scaled to the accent's (the orange #FF8A00 becomes
#38BDF8 itself). Neutral pixels stay as they are.

usage: python recolor-branding.py <in.png> <out.png>   (run once per file: ICON0.PNG, PIC1.PNG, banner.png)
"""

import sys

import numpy as np
from PIL import Image

TARGET = (0x38, 0xBD, 0xF8)


def hsv(rgb):
    r, g, b = (rgb[..., i] for i in range(3))
    mx, mn = rgb.max(-1), rgb.min(-1)
    d = mx - mn
    h = np.zeros_like(mx)
    nz = d > 1e-6
    rm = nz & (mx == r)
    gm = nz & (mx == g) & ~rm
    bm = nz & ~rm & ~gm
    h[rm] = ((g - b)[rm] / d[rm]) % 6
    h[gm] = (b - r)[gm] / d[gm] + 2
    h[bm] = (r - g)[bm] / d[bm] + 4
    h *= 60.0
    s = np.where(mx > 1e-6, d / np.maximum(mx, 1e-6), 0)
    return h, s, mx


def rgb_of(h, s, v):
    c = v * s
    hp = (h / 60.0) % 6
    x = c * (1 - np.abs(hp % 2 - 1))
    z = np.zeros_like(h)
    out = np.zeros(h.shape + (3,))
    for lo, (a, b_, c_) in enumerate(((c, x, z), (x, c, z), (z, c, x), (z, x, c), (x, z, c), (c, z, x))):
        m = (hp >= lo) & (hp < lo + 1)
        out[m] = np.stack([a[m], b_[m], c_[m]], -1)
    return out + (v - c)[..., None]


def main(src, dst):
    im = np.asarray(Image.open(src).convert("RGB"), dtype=np.float64) / 255.0
    h, s, v = hsv(im)
    th, ts, tv = hsv(np.array(TARGET, dtype=np.float64)[None, None, :] / 255.0)
    warm = (h <= 70.0) & (s > 0.02)
    s2 = np.where(warm, s * ts[0, 0], s)            # the orange's full saturation -> the accent's
    v2 = np.where(warm, v * (tv[0, 0] + (1 - tv[0, 0]) * (1 - s)), v)   # saturated pixels -> the accent's brightness
    h2 = np.where(warm, th[0, 0], h)
    out = np.clip(rgb_of(h2, s2, v2) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(out, "RGB").save(dst, optimize=True)
    print("wrote", dst)


main(sys.argv[1], sys.argv[2])

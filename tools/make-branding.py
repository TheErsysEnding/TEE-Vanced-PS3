#!/usr/bin/env python3
"""Render the XMB artwork for TEE Vanced PS3.

Two files, both consumed by the package build:
  ICON0.PNG  320x176  - the tile in the XMB game column
  PIC1.PNG  1920x1080 - the full-screen background the XMB shows while the tile is selected

The palette is the one the user's other tools already use (TEE Video2Audio, TEEconverter), so the console
entry looks like it belongs to the same family:
  #ff8a00 orange (brand), #ffbe5c light orange, #eadfce warm off-white, #9a8c74 muted, #14100a background.

The XMB draws the game's title and its own chrome over the LEFT half of PIC1, so everything that has to stay
readable lives on the right.

Since v2.0.0 both also say what the TEE Decoder brings (FULL HD 60 FPS, H.264) and carry the linktree where it is
seen - on the tile, and as a plate under the claim on the background (it used to be a faint line in a corner).
That text is Manrope, the app's own font, read straight from the bytes the app embeds - no installed font needed
for it; the wordmark stays Montserrat.
"""

import io
import pathlib
import re
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "branding"
OUT.mkdir(exist_ok=True)

BRAND = (255, 138, 0)
LIGHT = (255, 190, 92)
CREAM = (234, 223, 206)
MUTED = (154, 140, 116)
INK = (20, 16, 10)
BG_TOP = (32, 25, 15)
BG_BOTTOM = (16, 13, 8)

HEAVY = "/usr/share/fonts/opentype/montserrat/Montserrat-ExtraBold.otf"
MEDIUM = "/usr/share/fonts/opentype/montserrat/Montserrat-SemiBold.otf"

LINKTREE = "linktr.ee/theersysending"


def font(path, size):
    return ImageFont.truetype(path, size)


_manrope = {}


def manrope(weight, size):
    """Manrope (SIL OFL 1.1) as the app embeds it: apps/yo-player/src/assets/manrope-<weight>.c holds the TTF."""
    if weight not in _manrope:
        text = (ROOT / "apps/yo-player/src/assets" / f"manrope-{weight}.c").read_text(encoding="utf-8")
        body = text[text.index("{") + 1:text.rindex("}")]
        _manrope[weight] = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body))
    return ImageFont.truetype(io.BytesIO(_manrope[weight]), size)


def pill(draw, x, y, text, fnt, fill, ink, pad_x, height, outline=None, width=0):
    """A rounded label with its text centred on the capitals, not on the font's line box. Returns its right edge."""
    l, _, r, _ = draw.textbbox((0, 0), text, font=fnt)
    _, cap_top, _, cap_bottom = draw.textbbox((0, 0), "H", font=fnt)
    w = (r - l) + 2 * pad_x
    draw.rounded_rectangle((x, y, x + w, y + height), radius=height // 2, fill=fill, outline=outline, width=width)
    draw.text((x + pad_x - l, y + (height - (cap_bottom - cap_top)) / 2 - cap_top), text, font=fnt, fill=ink)
    return x + w


def draw_icon_extras(img):
    """The tile's claim across the top, and the linktree under the wordmark."""
    d = ImageDraw.Draw(img)
    end = pill(d, 22, 12, "FULL HD 60 FPS", manrope("extrabold", 14), BRAND, INK, 9, 24)
    d.text((end + 8, 17), "H.264 · TEE DECODER", font=manrope("extrabold", 12), fill=LIGHT)
    d.text((22, 143), LINKTREE, font=manrope("extrabold", 15), fill=CREAM)


def draw_background_extras(img):
    """Under the background's subtitle: the claim, and the linktree as a plate that can be seen from the sofa."""
    d = ImageDraw.Draw(img)
    x = 1306
    pill(d, x, 690, "FULL HD 60 FPS", manrope("extrabold", 34), BRAND, INK, 26, 64)
    d.text((x, 778), "H.264 SUPPORT · TEE DECODER", font=manrope("extrabold", 34), fill=LIGHT)
    d.text((x, 880), "Updates & more from TEE:", font=manrope("semibold", 24), fill=MUTED)
    pill(d, x, 922, LINKTREE, manrope("extrabold", 34), (34, 26, 16), CREAM, 26, 64, outline=BRAND, width=3)


def vertical_gradient(size, top, bottom):
    """A dithered vertical ramp.

    A dark gradient across 1080 rows spans only a handful of 8-bit values, so a straight ramp lands in wide
    flat bands with a visible step between them - very obvious on a TV, which is where this is shown. One
    step of ordered dither scatters the rounding error and the steps disappear.
    """
    w, h = size
    BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        exact = [top[i] + (bottom[i] - top[i]) * t for i in range(3)]
        for x in range(w):
            threshold = (BAYER[y & 3][x & 3] + 0.5) / 16.0
            px[x, y] = tuple(int(exact[i]) + (1 if (exact[i] - int(exact[i])) > threshold else 0) for i in range(3))
    return img


def glow(size, centre, radius, colour, strength):
    """A soft radial light, drawn as a blurred disc so it has no visible edge."""
    layer = Image.new("L", size, 0)
    d = ImageDraw.Draw(layer)
    d.ellipse([centre[0] - radius, centre[1] - radius, centre[0] + radius, centre[1] + radius], fill=strength)
    layer = layer.filter(ImageFilter.GaussianBlur(radius * 0.55))
    tint = Image.new("RGB", size, colour)
    return tint, layer


def play_badge(draw, box, radius, fill, triangle):
    """The rounded square with a play triangle - the app's mark, used at both sizes."""
    x0, y0, x1, y1 = box
    draw.rounded_rectangle(box, radius=radius, fill=fill)
    w, h = x1 - x0, y1 - y0
    cx, cy = x0 + w * 0.56, y0 + h / 2
    s = h * 0.30
    draw.polygon([(cx - s * 0.72, cy - s), (cx - s * 0.72, cy + s), (cx + s * 0.86, cy)], fill=triangle)


def tracked_text(draw, xy, text, fnt, fill, tracking):
    """Letter-spaced text: Pillow has no tracking, and the wordmark needs it to read as a logo."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill)
        x += draw.textlength(ch, font=fnt) + tracking
    return x - tracking


def tracked_width(draw, text, fnt, tracking):
    return sum(draw.textlength(c, font=fnt) for c in text) + tracking * (len(text) - 1)


def make_icon():
    W, H = 320, 176
    img = vertical_gradient((W, H), BG_TOP, BG_BOTTOM).convert("RGB")
    tint, mask = glow((W, H), (58, 88), 120, (255, 120, 0), 90)
    img = Image.composite(Image.blend(img, tint, 0.55), img, mask)

    d = ImageDraw.Draw(img)
    play_badge(d, (22, 54, 90, 122), 20, BRAND, (20, 16, 10))

    tee = font(HEAVY, 62)
    vanced = font(MEDIUM, 23)
    d.text((108, 44), "TEE", font=tee, fill=BRAND)
    tracked_text(d, (111, 110), "VANCED", vanced, CREAM, 3.2)

    # no version on the tile: it went stale with every release (the XMB title carries it instead)
    draw_icon_extras(img)

    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=4, outline=(60, 46, 28), width=1)
    path = OUT / "ICON0.PNG"
    img.save(path, optimize=True)
    return path, img.size


def make_background():
    W, H = 1920, 1080
    img = vertical_gradient((W, H), (26, 20, 12), (11, 9, 6)).convert("RGB")
    tint, mask = glow((W, H), (1330, 470), 620, (255, 120, 0), 66)
    img = Image.composite(Image.blend(img, tint, 0.5), img, mask)

    # an oversized, very faint mark bleeding off the right edge: texture, not decoration
    ghost = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(ghost)
    play_badge(gd, (1500, 250, 2100, 850), 150, (255, 138, 0, 26), (0, 0, 0, 0))
    img = Image.alpha_composite(img.convert("RGBA"), ghost).convert("RGB")

    d = ImageDraw.Draw(img)
    play_badge(d, (1140, 372, 1268, 500), 34, BRAND, (18, 14, 9))

    tee = font(HEAVY, 132)
    vanced = font(MEDIUM, 52)
    d.text((1300, 348), "TEE", font=tee, fill=BRAND)
    tracked_text(d, (1306, 500), "VANCED", vanced, CREAM, 7.5)

    d.rectangle((1306, 585, 1306 + 300, 589), fill=BRAND)

    sub = font(MEDIUM, 26)
    # The one line in the project that would read as trademark USE rather than description, so it does not
    # name anyone else's brand. "YT" is the owner's wording.
    d.text((1306, 616), "YT Player for PlayStation 3 with adblock", font=sub, fill=MUTED)

    draw_background_extras(img)

    path = OUT / "PIC1.PNG"
    img.save(path, optimize=True)
    return path, img.size


if __name__ == "__main__":
    for path, size in (make_icon(), make_background()):
        print(f"{path.name:10} {size[0]}x{size[1]}  {path.stat().st_size:>8} bytes  {path}")

# make-flags - the flags of the language picker (V3.1.0, the same as TEE Witch's) into apps/yo-player/src/assets/flags.c.
#
#   python -I tools/make-flags.py [preview-dir]
#
# Drawn here from their geometry (needs Pillow), 96x64 PNG each, drawn four times as big and scaled down for
# smooth edges. Simplified where a flag has fine detail (no coats of arms, Brazil without its stars and motto):
# at this size that detail would be noise. Flags are not anyone's copyright; no image is taken from anywhere.
import io, math, os, sys
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, 'apps', 'yo-player', 'src', 'assets', 'flags.c')
W, H, S = 96, 64, 4
BW, BH = W * S, H * S


def hstripes(d, colors, weights=None):
    weights = weights or [1] * len(colors)
    total, y = sum(weights), 0.0
    for c, w in zip(colors, weights):
        h = BH * w / total
        d.rectangle([0, round(y), BW, round(y + h)], fill=c)
        y += h


def vstripes(d, colors):
    for i, c in enumerate(colors):
        d.rectangle([round(BW * i / len(colors)), 0, round(BW * (i + 1) / len(colors)), BH], fill=c)


def star(d, cx, cy, r, color, rotation=-90.0, inner=0.382):
    pts = []
    for k in range(10):
        a = math.radians(rotation + k * 36)
        rr = r if k % 2 == 0 else r * inner
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=color)


def circle(d, cx, cy, r, color):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)


def thick_line(d, p, q, width, color):
    (x0, y0), (x1, y1) = p, q
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    ox, oy = -dy / n * width / 2, dx / n * width / 2
    d.polygon([(x0 + ox, y0 + oy), (x1 + ox, y1 + oy), (x1 - ox, y1 - oy), (x0 - ox, y0 - oy)], fill=color)


def gb(img, d):
    d.rectangle([0, 0, BW, BH], fill='#012169')
    for p, q in (((0, 0), (BW, BH)), ((0, BH), (BW, 0))):
        thick_line(d, p, q, BH * 0.2, 'white')
    for p, q in (((0, 0), (BW, BH)), ((0, BH), (BW, 0))):
        thick_line(d, p, q, BH * 0.067, '#C8102E')
    d.rectangle([BW / 2 - BH / 6, 0, BW / 2 + BH / 6, BH], fill='white')
    d.rectangle([0, BH / 3, BW, BH * 2 / 3], fill='white')
    d.rectangle([BW / 2 - BH / 10, 0, BW / 2 + BH / 10, BH], fill='#C8102E')
    d.rectangle([0, BH * 0.4, BW, BH * 0.6], fill='#C8102E')


def br(img, d):
    d.rectangle([0, 0, BW, BH], fill='#009C3B')
    d.polygon([(BW * 0.085, BH / 2), (BW / 2, BH * 0.12), (BW * 0.915, BH / 2), (BW / 2, BH * 0.88)], fill='#FFDF00')
    cx, cy, r = BW / 2, BH / 2, BH * 0.25
    circle(d, cx, cy, r, '#002776')
    band = Image.new('L', (BW, BH), 0)
    bd = ImageDraw.Draw(band)
    R = r * 2.4
    bx, by = cx - r * 0.5, cy + r * 2.25
    bd.ellipse([bx - R, by - R, bx + R, by + R], fill=255)
    bd.ellipse([bx - R + r * 0.2, by - R + r * 0.2, bx + R - r * 0.2, by + R - r * 0.2], fill=0)
    disc = Image.new('L', (BW, BH), 0)
    ImageDraw.Draw(disc).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    mask = Image.composite(band, Image.new('L', (BW, BH), 0), disc)   # the band, inside the circle only
    img.paste('white', (0, 0), mask)


def tr_(img, d):
    d.rectangle([0, 0, BW, BH], fill='#E30A17')
    circle(d, BH * 0.5, BH / 2, BH * 0.25, 'white')
    circle(d, BH * 0.5 + BH * 0.0625, BH / 2, BH * 0.2, '#E30A17')
    star(d, BH * 0.5 + BH * 0.333, BH / 2, BH * 0.125, 'white', rotation=180)


def se(img, d):
    d.rectangle([0, 0, BW, BH], fill='#006AA7')
    d.rectangle([BW * 5 / 16, 0, BW * 7 / 16, BH], fill='#FECC00')
    d.rectangle([0, BH * 0.4, BW, BH * 0.6], fill='#FECC00')


def cz(img, d):
    hstripes(d, ['white', '#D7141A'])
    d.polygon([(0, 0), (0, BH), (BW / 2, BH / 2)], fill='#11457E')


def gr(img, d):
    hstripes(d, ['#0D5EAF', 'white'] * 4 + ['#0D5EAF'])
    s = BH * 5 / 9
    d.rectangle([0, 0, s, s], fill='#0D5EAF')
    d.rectangle([s * 2 / 5, 0, s * 3 / 5, s], fill='white')
    d.rectangle([0, s * 2 / 5, s, s * 3 / 5], fill='white')


def vn(img, d):
    d.rectangle([0, 0, BW, BH], fill='#DA251D')
    star(d, BW / 2, BH / 2 + BH * 0.02, BH * 0.3, '#FFFF00')


def jp(img, d):
    d.rectangle([0, 0, BW, BH], fill='white')
    circle(d, BW / 2, BH / 2, BH * 0.3, '#BC002D')


def kr(img, d):
    d.rectangle([0, 0, BW, BH], fill='white')
    cx, cy, r = BW / 2, BH / 2, BH * 0.25
    emblem = Image.new('RGBA', (BW, BH), (0, 0, 0, 0))
    ed = ImageDraw.Draw(emblem)
    ed.pieslice([cx - r, cy - r, cx + r, cy + r], 180, 360, fill='#CD2E3A')
    ed.pieslice([cx - r, cy - r, cx + r, cy + r], 0, 180, fill='#0047A0')
    circle(ed, cx - r / 2, cy, r / 2, '#CD2E3A')
    circle(ed, cx + r / 2, cy, r / 2, '#0047A0')
    emblem = emblem.rotate(-33.7, center=(cx, cy), resample=Image.BICUBIC)
    img.alpha_composite(emblem)
    # the four trigrams, each across its diagonal (1 a whole bar, 0 a broken one)
    grams = {(-1, -1): (1, 1, 1), (1, 1): (0, 0, 0), (1, -1): (0, 1, 0), (-1, 1): (1, 0, 1)}
    bar_w, bar_h, gap = r * 1.0, r * 0.13, r * 0.09
    for (sx, sy), bars in grams.items():
        g = Image.new('RGBA', (int(bar_w * 1.6), int(bar_w * 1.6)), (0, 0, 0, 0))
        gd = ImageDraw.Draw(g)
        gx0 = (g.width - bar_w) / 2
        gy0 = (g.height - (3 * bar_h + 2 * gap)) / 2
        for k, whole in enumerate(bars):
            y = gy0 + k * (bar_h + gap)
            if whole: gd.rectangle([gx0, y, gx0 + bar_w, y + bar_h], fill='black')
            else:
                gd.rectangle([gx0, y, gx0 + bar_w * 0.45, y + bar_h], fill='black')
                gd.rectangle([gx0 + bar_w * 0.55, y, gx0 + bar_w, y + bar_h], fill='black')
        ang = math.degrees(math.atan2(BH, BW))
        g = g.rotate(-ang if sx * sy > 0 else ang, resample=Image.BICUBIC, expand=False)
        dist = r * 1.75
        u = math.hypot(BW, BH)
        px, py = cx + sx * dist * BW / u, cy + sy * dist * BH / u
        img.alpha_composite(g, (int(px - g.width / 2), int(py - g.height / 2)))


def cn(img, d):
    d.rectangle([0, 0, BW, BH], fill='#EE1C25')
    u = BH / 20
    star(d, 5 * u, 5 * u, 3 * u, '#FFFF00')
    for x, y in ((10, 2), (12, 4), (12, 7), (10, 9)):
        ang = math.degrees(math.atan2(5 - y, 5 - x))
        star(d, x * u, y * u, u, '#FFFF00', rotation=ang)


def ae(img, d):
    hstripes(d, ['#00732F', 'white', 'black'])
    d.rectangle([0, 0, BW / 4, BH], fill='#FF0000')


FLAGS = {
    'gb': gb,
    'de': lambda i, d: hstripes(d, ['#000000', '#DD0000', '#FFCE00']),
    'es': lambda i, d: hstripes(d, ['#AA151B', '#F1BF00', '#AA151B'], [1, 2, 1]),
    'fr': lambda i, d: vstripes(d, ['#0055A4', 'white', '#EF4135']),
    'it': lambda i, d: vstripes(d, ['#009246', 'white', '#CE2B37']),
    'br': br,
    'pl': lambda i, d: hstripes(d, ['white', '#DC143C']),
    'tr': tr_,
    'nl': lambda i, d: hstripes(d, ['#AE1C28', 'white', '#21468B']),
    'se': se,
    'cz': cz,
    'ro': lambda i, d: vstripes(d, ['#002B7F', '#FCD116', '#CE1126']),
    'hu': lambda i, d: hstripes(d, ['#CD2A3E', 'white', '#436F4D']),
    'gr': gr,
    'ru': lambda i, d: hstripes(d, ['white', '#0039A6', '#D52B1E']),
    'ua': lambda i, d: hstripes(d, ['#0057B7', '#FFD700']),
    'vn': vn,
    'id': lambda i, d: hstripes(d, ['#FF0000', 'white']),
    'jp': jp,
    'kr': kr,
    'cn': cn,
    'ae': ae,
}


def render(code):
    img = Image.new('RGBA', (BW, BH), (0, 0, 0, 0))
    FLAGS[code](img, ImageDraw.Draw(img))
    img = img.resize((W, H), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, 'PNG', optimize=True)
    return img, buf.getvalue()


def main():
    preview = sys.argv[1] if len(sys.argv) > 1 else None
    out = ['// generated by tools/make-flags.py - do not edit; run the tool again.',
           '// The language picker\'s flags (V3.1.0), 96x64 PNG, drawn from their geometry.', '']
    sheet = Image.new('RGBA', ((W + 8) * 6, (H + 8) * ((len(FLAGS) + 5) // 6)), (40, 40, 48, 255))
    for k, code in enumerate(FLAGS):
        img, png = render(code)
        name = 'flag' + code[:1].upper() + code[1:]
        out.append('const unsigned char %sData[] = {' % name)
        for i in range(0, len(png), 16):
            out.append('  ' + ', '.join('0x%02x' % b for b in png[i:i + 16]) + ',')
        out.append('};')
        out.append('const unsigned int %sDataSize = sizeof %sData;' % (name, name))
        out.append('')
        sheet.alpha_composite(img, ((k % 6) * (W + 8) + 4, (k // 6) * (H + 8) + 4))
    open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('%d flags into %s' % (len(FLAGS), OUT))
    if preview:
        os.makedirs(preview, exist_ok=True)
        sheet.save(os.path.join(preview, 'flags-sheet.png'))
        print('preview', os.path.join(preview, 'flags-sheet.png'))


if __name__ == '__main__':
    main()

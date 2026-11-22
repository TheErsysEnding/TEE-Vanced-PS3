# subset-font - a TrueType font cut down to the characters the app needs from it, as a C array (V3.1.0).
#
#   python -I tools/subset-font.py <DejaVuSans-Bold.ttf> <out.c> <symbol> [note]
#
# For the Arabic (and Hebrew) text: DejaVu Sans Bold whole is 0.7 MB in the app and another 0.7 MB once opened (font.c
# keeps a copy) - with 4K playing the console has about 3 MB left. Kept: Basic Latin, Latin-1, Latin Extended-A (names
# in a title), General Punctuation, Hebrew, Arabic, Arabic Supplement and both Arabic Presentation Forms blocks (the
# renderer draws Arabic in its joined forms, font.c). Every other glyph goes; so do the tables that list glyphs by
# number for another purpose (GSUB/GPOS/GDEF/kern and the like) - the renderer uses none of them. The glyphs, their
# hinting and the names of the font are as DejaVu made them: the Bitstream Vera license allows a changed font so long
# as its name holds neither "Bitstream" nor "Vera" ("DejaVu Sans" holds neither) - see DejaVu-LICENSE.txt beside it.
import struct, sys

RANGES = [(0x0020, 0x007E), (0x00A0, 0x017F), (0x2000, 0x206F), (0x20AC, 0x20AC), (0x0590, 0x05FF),
          (0x0600, 0x06FF), (0x0750, 0x077F), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF)]
KEEP = {'head', 'hhea', 'maxp', 'OS/2', 'name', 'cvt ', 'fpgm', 'prep', 'gasp'}   # + cmap, hmtx, loca, glyf, post rebuilt


def tables_of(font):
    n = struct.unpack('>H', font[4:6])[0]
    out = {}
    for i in range(n):
        tag, _, off, length = struct.unpack('>4sIII', font[12 + 16 * i:28 + 16 * i])
        out[tag.decode('latin-1')] = font[off:off + length]
    return out


def cmap_of(cmap):
    best = None
    n = struct.unpack('>H', cmap[2:4])[0]
    for i in range(n):
        pid, eid, off = struct.unpack('>HHI', cmap[4 + 8 * i:12 + 8 * i])
        fmt = struct.unpack('>H', cmap[off:off + 2])[0]
        if (pid, eid, fmt) in ((3, 10, 12), (0, 4, 12)): best = (fmt, off); break
        if (pid, eid, fmt) in ((3, 1, 4), (0, 3, 4)) and best is None: best = (fmt, off)
    fmt, off = best
    m = {}
    if fmt == 4:
        segs = struct.unpack('>H', cmap[off + 6:off + 8])[0] // 2
        ends = struct.unpack('>%dH' % segs, cmap[off + 14:off + 14 + 2 * segs])
        p = off + 16 + 2 * segs
        starts = struct.unpack('>%dH' % segs, cmap[p:p + 2 * segs]); p += 2 * segs
        deltas = struct.unpack('>%dh' % segs, cmap[p:p + 2 * segs]); p += 2 * segs
        ro_pos = p
        offs = struct.unpack('>%dH' % segs, cmap[p:p + 2 * segs])
        for s in range(segs):
            for c in range(starts[s], ends[s] + 1):
                if c == 0xFFFF: continue
                if offs[s] == 0: g = (c + deltas[s]) & 0xFFFF
                else:
                    a = ro_pos + 2 * s + offs[s] + 2 * (c - starts[s])
                    g = struct.unpack('>H', cmap[a:a + 2])[0]
                    if g: g = (g + deltas[s]) & 0xFFFF
                if g: m[c] = g
    else:
        groups = struct.unpack('>I', cmap[off + 12:off + 16])[0]
        for k in range(groups):
            s, e, g0 = struct.unpack('>III', cmap[off + 16 + 12 * k:off + 28 + 12 * k])
            for c in range(s, e + 1): m[c] = g0 + (c - s)
    return m


def glyph_data(glyf, loca, long_loca, g):
    if long_loca: a, b = struct.unpack('>II', loca[4 * g:4 * g + 8])
    else:
        a, b = struct.unpack('>HH', loca[2 * g:2 * g + 4]); a *= 2; b *= 2
    return glyf[a:b]


def components(data):
    # a composite glyph's (flags offset, glyph index) pairs
    if len(data) < 10 or struct.unpack('>h', data[0:2])[0] >= 0: return []
    out, p = [], 10
    while True:
        flags, gi = struct.unpack('>HH', data[p:p + 4])
        out.append((p + 2, gi))
        p += 4
        p += 4 if flags & 0x0001 else 2                      # ARG_1_AND_2_ARE_WORDS
        if flags & 0x0008: p += 2                            # WE_HAVE_A_SCALE
        elif flags & 0x0040: p += 4                          # X_AND_Y_SCALE
        elif flags & 0x0080: p += 8                          # TWO_BY_TWO
        if not flags & 0x0020: break                         # MORE_COMPONENTS
    return out


def checksum(data):
    data = data + b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF


def cmap4(mapping):
    codes = sorted(c for c in mapping if c <= 0xFFFF)
    segs = []
    for c in codes:
        if segs and c == segs[-1][1] + 1 and mapping[c] == mapping[segs[-1][1]] + 1: segs[-1][1] = c
        else: segs.append([c, c])
    segs.append([0xFFFF, 0xFFFF])
    n = len(segs)
    ends = [e for _, e in segs]
    starts = [s for s, _ in segs]
    deltas = [((mapping[s] - s) & 0xFFFF) if s != 0xFFFF else 1 for s, _ in segs]
    search = 2 * (1 << (n.bit_length() - 1))
    sub = struct.pack('>HHHHHHH', 4, 0, 0, 2 * n, search, (n.bit_length() - 1), 2 * n - search)
    sub += struct.pack('>%dH' % n, *ends) + b'\0\0' + struct.pack('>%dH' % n, *starts)
    sub += struct.pack('>%dH' % n, *deltas) + struct.pack('>%dH' % n, *([0] * n))
    sub = sub[:2] + struct.pack('>H', len(sub)) + sub[4:]
    return struct.pack('>HH', 0, 2) + struct.pack('>HHI', 0, 3, 20) + struct.pack('>HHI', 3, 1, 20) + sub


def main():
    src, out_c, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
    note = sys.argv[4] if len(sys.argv) > 4 else ''
    font = open(src, 'rb').read()
    t = tables_of(font)
    long_loca = struct.unpack('>h', t['head'][50:52])[0] == 1
    num = struct.unpack('>H', t['maxp'][4:6])[0]
    nh = struct.unpack('>H', t['hhea'][34:36])[0]
    full = cmap_of(t['cmap'])
    wanted = {c: g for c, g in full.items() if any(a <= c <= b for a, b in RANGES)}
    keep = {0} | set(wanted.values())
    todo = list(keep)
    while todo:   # the parts composite glyphs are made of
        g = todo.pop()
        for _, gi in components(glyph_data(t['glyf'], t['loca'], long_loca, g)):
            if gi not in keep: keep.add(gi); todo.append(gi)
    order = sorted(keep)
    new_of = {g: i for i, g in enumerate(order)}

    glyf, loca = bytearray(), []
    hmtx = bytearray()
    for g in order:
        loca.append(len(glyf))
        data = bytearray(glyph_data(t['glyf'], t['loca'], long_loca, g))
        for pos, gi in components(bytes(data)): struct.pack_into('>H', data, pos, new_of[gi])
        glyf += data + b'\0' * (-len(data) % 4)
        adv, lsb = struct.unpack('>Hh', t['hmtx'][4 * min(g, nh - 1):4 * min(g, nh - 1) + 4])
        if g >= nh: lsb = struct.unpack('>h', t['hmtx'][4 * nh + 2 * (g - nh):4 * nh + 2 * (g - nh) + 2])[0]
        hmtx += struct.pack('>Hh', adv, lsb)
    loca.append(len(glyf))

    tables = {k: bytearray(v) for k, v in t.items() if k in KEEP}
    struct.pack_into('>h', tables['head'], 50, 1)                       # long loca
    struct.pack_into('>I', tables['head'], 8, 0)                        # checkSumAdjustment, set below
    struct.pack_into('>H', tables['hhea'], 34, len(order))
    struct.pack_into('>H', tables['maxp'], 4, len(order))
    tables['glyf'] = bytes(glyf)
    tables['loca'] = struct.pack('>%dI' % len(loca), *loca)
    tables['hmtx'] = bytes(hmtx)
    tables['cmap'] = cmap4({c: new_of[g] for c, g in wanted.items()})
    tables['post'] = bytearray(t['post'][:32]); struct.pack_into('>I', tables['post'], 0, 0x00030000)   # no glyph names

    tags = sorted(tables)
    n = len(tags)
    es = n.bit_length() - 1
    head = struct.pack('>IHHHH', 0x00010000, n, 16 * (1 << es), es, 16 * n - 16 * (1 << es))
    offset = 12 + 16 * n
    dir_, body = b'', b''
    for tag in tags:
        data = bytes(tables[tag])
        dir_ += struct.pack('>4sIII', tag.encode('latin-1'), checksum(data), offset + len(body), len(data))
        body += data + b'\0' * (-len(data) % 4)
    ttf = bytearray(head + dir_ + body)
    adj = (0xB1B0AFBA - checksum(bytes(ttf))) & 0xFFFFFFFF
    head_off = struct.unpack('>I', dir_[16 * tags.index('head') + 8:16 * tags.index('head') + 12])[0]
    struct.pack_into('>I', ttf, head_off + 8, adj)

    lines = ['// generated by tools/subset-font.py - do not edit; run the tool again.',
             '// %s' % note if note else '//',
             '// %d of %d glyphs (%d characters): Latin, punctuation, Hebrew, Arabic and its presentation forms.' % (
                 len(order), num, len(wanted)), '',
             'const unsigned char %sData[] = {' % symbol]
    for i in range(0, len(ttf), 16):
        lines.append('  ' + ', '.join('0x%02x' % b for b in ttf[i:i + 16]) + ',')
    lines += ['};', 'const unsigned int %sDataSize = sizeof %sData;' % (symbol, symbol), '']
    open(out_c, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    if len(sys.argv) > 5: open(sys.argv[5], 'wb').write(bytes(ttf))
    print('%s: %d of %d glyphs, %d characters, %d bytes (was %d)' % (out_c, len(order), num, len(wanted), len(ttf), len(font)))


if __name__ == '__main__':
    main()

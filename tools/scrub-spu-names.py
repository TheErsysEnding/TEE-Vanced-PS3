#!/usr/bin/env python3
"""Scrub the build machine's path out of the TEE Decoder's embedded SPU programs.

The building block's td_spu_images.c carries the two SPU programs as C arrays. Each is an ELF whose
.note.spu_name section names the program by the full path it was linked to on the build machine - a home
directory, with the user's name in it. This replaces that path, in place and at the same length, with the
program's bare file name followed by NULs: only that string changes, no instruction and no array length.

    python3 tools/scrub-spu-names.py libs/simple-lib-av/src/tee_decoder/td_spu_images.c

Run it after every refresh of the module folder, then update libs/simple-lib-av/tee_decoder.sha256.
It refuses anything it does not recognise, and is a no-op on a file it already scrubbed.
"""
import re
import struct
import sys

path = sys.argv[1]
with open(path, encoding="utf-8", newline="") as f:
    source = f.read()

ARRAY = re.compile(r"(const unsigned char (\w+)\[(\d+)\][^=]*=\s*\{)(.*?)(\};)", re.S)


def note_name_span(data):
    """(start, length) of the .note.spu_name descriptor's string inside an SPU ELF, or None."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:   # ELF32, big endian
        raise SystemExit("not a big-endian ELF32 SPU program")
    shoff = struct.unpack(">I", data[0x20:0x24])[0]
    shentsize, shnum, shstrndx = struct.unpack(">HHH", data[0x2E:0x34])
    sections = [struct.unpack(">IIIIIIIIII", data[shoff + i * shentsize:shoff + i * shentsize + 40]) for i in range(shnum)]
    strtab = sections[shstrndx]
    names = data[strtab[4]:strtab[4] + strtab[5]]
    for s in sections:
        name = names[s[0]:names.index(b"\0", s[0])]
        if name != b".note.spu_name":
            continue
        offset, size = s[4], s[5]
        namesz, descsz, _ = struct.unpack(">III", data[offset:offset + 12])
        desc = offset + 12 + ((namesz + 3) & ~3)
        if desc + descsz > offset + size:
            raise SystemExit(".note.spu_name is malformed")
        return desc, descsz
    return None


changed = 0


def scrub(match):
    global changed
    head, name, length, body, tail = match.groups()
    numbers = re.findall(r"0x([0-9a-fA-F]{2})", body)
    data = bytearray(int(n, 16) for n in numbers)
    if len(data) != int(length):
        raise SystemExit("%s: %d bytes listed, %s declared" % (name, len(data), length))
    span = note_name_span(bytes(data))
    if not span:
        return match.group(0)
    start, size = span
    text = bytes(data[start:start + size]).split(b"\0")[0]
    base = text.rsplit(b"/", 1)[-1]
    if text == base:
        return match.group(0)   # already only a file name
    data[start:start + size] = base + b"\0" * (size - len(base))
    changed += 1
    # write the bytes back in the file's own layout: 24 a line, tab-indented, as bin2c.py does
    lines = []
    for i in range(0, len(data), 24):
        lines.append("\t" + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    return head + "\n" + "\n".join(lines) + "\n" + tail


result = ARRAY.sub(scrub, source)
if result.count("const unsigned char ") != source.count("const unsigned char "):
    raise SystemExit("arrays lost while rewriting")
with open(path, "w", encoding="utf-8", newline="") as f:
    f.write(result)
print("scrubbed %d SPU program name(s) in %s" % (changed, path))

#!/usr/bin/env python3
"""Patch TITLE string inside a PS3 PARAM.SFO (utf-8 value, fixed slot size).

build-pkg-fromsource.sh no longer uses this - it builds PARAM.SFO from apps/yo-player/PARAM.SFO.xml with
make-param-sfo.py, which needs no extracted package. This stays for patching an SFO that already exists.

usage: patch-param-title.py [src PARAM.SFO] [dst PARAM.SFO] [new title]
"""

import os
import struct
import sys

OLD_TITLE = b"Yo! Player"
NEW_TITLE = b"yo player fixxed by TheErsysEnding Dont Use VPN"


def patch_sfo(data: bytearray, old: bytes, new: bytes) -> None:
    idx = data.find(old)
    if idx < 0:
        raise ValueError(f"title {old!r} not found in SFO")
    # TITLE value is null-padded to the next key boundary; find run of bytes after old title.
    end = idx
    while end < len(data) and data[end] != 0:
        end += 1
    # include following null padding in the slot we may reuse
    slot_end = end
    while slot_end < len(data) and data[slot_end] == 0:
        slot_end += 1
    slot_size = slot_end - idx
    if len(new) >= slot_size:
        raise ValueError(f"new title too long ({len(new)} >= slot {slot_size})")
    data[idx : idx + slot_size] = new + b"\x00" * (slot_size - len(new))

    # The index entry pointing at this slot records how many bytes of it are used (title + null). Patching
    # the bytes alone left that at len("Yo! Player") + 1 = 11 - it is 11 in the released v0.12 package. The
    # XMB reads up to the null and never showed it, but the field was wrong. Find the entry by its data offset
    # and fix it.
    _, _, _, data_table, count = struct.unpack_from("<IIIII", data, 0)
    for i in range(count):
        entry = 0x14 + i * 0x10
        _, fmt, _, _, offset = struct.unpack_from("<HHIII", data, entry)
        if data_table + offset == idx:
            struct.pack_into("<I", data, entry + 4, len(new) + 1 if fmt == 0x0204 else len(new))
            break


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "extracted/PARAM.SFO"
    dst = sys.argv[2] if len(sys.argv) > 2 else "out/PARAM.SFO"
    # build-pkg-fromsource.sh hands the title in as the third argument and as PKG_TITLE; the copy of this
    # script in the repository read neither and always wrote NEW_TITLE.
    if len(sys.argv) > 3:
        new_title = sys.argv[3].encode("utf-8")
    elif os.environ.get("PKG_TITLE"):
        new_title = os.environ["PKG_TITLE"].encode("utf-8")
    else:
        new_title = NEW_TITLE
    data = bytearray(open(src, "rb").read())
    patch_sfo(data, OLD_TITLE, new_title)
    open(dst, "wb").write(data)
    print(f"patched TITLE -> {new_title.decode()!r}")
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

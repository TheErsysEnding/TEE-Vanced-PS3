#!/usr/bin/env python3
"""Patch TITLE string inside a PS3 PARAM.SFO (utf-8 value, fixed slot size)."""

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


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "extracted/PARAM.SFO"
    dst = sys.argv[2] if len(sys.argv) > 2 else "out/PARAM.SFO"
    data = bytearray(open(src, "rb").read())
    patch_sfo(data, OLD_TITLE, NEW_TITLE)
    open(dst, "wb").write(data)
    print(f"patched TITLE -> {NEW_TITLE.decode()!r}")
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

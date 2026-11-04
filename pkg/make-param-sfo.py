#!/usr/bin/env python3
"""Build PARAM.SFO from apps/yo-player/PARAM.SFO.xml.

A port of mohasi's tools/xml-to-sfo (C#, .NET 4) to Python, so the package can be put together from any
machine. Before this the build took a PARAM.SFO extracted from mohasi's own released package and patched the
title into it - a file that is in nobody's checkout but the one it was extracted on.

Same XML schema and the same binary layout as xml-to-sfo:

  header     0x14 bytes  - magic, version, table offsets, entry count
  index      0x10 bytes per entry
  key table  - null-terminated keys, padded to 4-byte alignment
  data table - one slot per entry; the slot is maxlength bytes, zero-padded

Entries are sorted by key, as Sony's tools write them. Given the v0.12 title, this reproduces the PARAM.SFO
inside the released TEE-Vanced-PS3-v0.12-beta.pkg byte for byte but for one field: TITLE's used length, which
the old in-place title patch left at 11, the length of "Yo! Player" (see patch-param-title.py).

usage: make-param-sfo.py <PARAM.SFO.xml> <out PARAM.SFO> [--title TEXT] [--app-ver TEXT]
"""

import argparse
import re
import struct
import sys
import xml.etree.ElementTree as ET

MAGIC = 0x46535000          # "\0PSF"
VERSION = 0x00000101        # 1.1
HEADER_SIZE = 0x14
INDEX_ENTRY_SIZE = 0x10

UTF8_SPECIAL = 0x0004       # utf8, no trailing null
UTF8 = 0x0204               # utf8, null-terminated
INT4 = 0x0404               # 32-bit little-endian integer

TYPE_NAMES = {"utf8": UTF8, "utf8-special": UTF8_SPECIAL, "int4": INT4}

# default type and slot size per well-known key, so the XML does not have to repeat them - the same table as
# xml-to-sfo's (psdevwiki PARAM.SFO and the SDK samples)
WELL_KNOWN = {
    "ACCOUNT_ID": (UTF8_SPECIAL, 16),
    "ACCOUNTID": (UTF8, 16),
    "ANALOG_MODE": (INT4, 4),
    "APP_VER": (UTF8, 8),
    "ATTRIBUTE": (INT4, 4),
    "BOOTABLE": (INT4, 4),
    "CATEGORY": (UTF8, 4),
    "CONTENT_ID": (UTF8, 48),
    "DETAIL": (UTF8, 1024),
    "GAMEDATA_ID": (UTF8, 32),
    "ITEM_PRIORITY": (INT4, 4),
    "LANG": (INT4, 4),
    "LICENSE": (UTF8, 512),
    "NP_COMMUNICATION_ID": (UTF8, 16),
    "NPCOMMID": (UTF8, 16),
    "PADDING": (UTF8_SPECIAL, 8),
    "PARAMS": (UTF8_SPECIAL, 1024),
    "PARAMS2": (UTF8_SPECIAL, 12),
    "PARENTAL_LEVEL": (INT4, 4),
    "PARENTAL_LEVEL_A": (INT4, 4),
    "PARENTAL_LEVEL_C": (INT4, 4),
    "PARENTAL_LEVEL_E": (INT4, 4),
    "PARENTAL_LEVEL_H": (INT4, 4),
    "PARENTAL_LEVEL_J": (INT4, 4),
    "PARENTAL_LEVEL_K": (INT4, 4),
    "PARENTALLEVEL": (INT4, 4),
    "PATCH_FILE": (UTF8, 32),
    "PS3_SYSTEM_VER": (UTF8, 8),
    "REGION_DENY": (INT4, 4),
    "RESOLUTION": (INT4, 4),
    "SAVEDATA_DETAIL": (UTF8, 1024),
    "SAVEDATA_DIRECTORY": (UTF8, 64),
    "SAVEDATA_FILE_LIST": (UTF8_SPECIAL, 3168),
    "SAVEDATA_LIST_PARAM": (UTF8, 8),
    "SAVEDATA_PARAMS": (UTF8_SPECIAL, 128),
    "SAVEDATA_TITLE": (UTF8, 128),
    "SOUND_FORMAT": (INT4, 4),
    "SOURCE": (INT4, 4),
    "SUB_TITLE": (UTF8, 128),
    "TARGET_APP_VER": (UTF8, 8),
    "TITLE": (UTF8, 128),
    "TITLE_ID": (UTF8, 16),
    "VERSION": (UTF8, 8),
    "XMB_APPS": (INT4, 4),
}

# keys are uppercase letters, digits and underscores; the parser would take anything, the PS3 would not
KEY_PATTERN = re.compile(r"^[A-Z][A-Z0-9_]*$")


class SfoError(Exception):
    pass


def parse_int(raw):
    """Decimal (63) or hex (0x3F), as a signed 32-bit value."""
    try:
        if raw[:2].lower() == "0x":
            value = int(raw[2:], 16)
            if not 0 <= value <= 0xFFFFFFFF:
                raise ValueError
            return value - (1 << 32) if value >= 1 << 31 else value
        value = int(raw, 10)
        if not -(1 << 31) <= value < 1 << 31:
            raise ValueError
        return value
    except ValueError:
        return None


def parse(path, overrides):
    """-> list of (key, type, maxlength, payload bytes), in file order."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise SfoError(f"XML is not well-formed: {error}")
    if root.tag != "paramsfo":
        raise SfoError("root element must be <paramsfo>")

    params = []
    seen = set()
    for element in root:
        if not isinstance(element.tag, str):
            continue                                    # comments
        if element.tag != "param":
            raise SfoError(f"unexpected element <{element.tag}>; only <param> is allowed inside <paramsfo>")
        key = element.get("key")
        if key is None:
            raise SfoError("<param> is missing required attribute 'key'")
        if not KEY_PATTERN.match(key):
            raise SfoError(f"key '{key}' must be uppercase letters, digits and underscores (e.g. TITLE_ID)")
        if key in seen:
            raise SfoError(f"duplicate key '{key}'")
        seen.add(key)

        known = WELL_KNOWN.get(key)
        type_name = element.get("type")
        if type_name is None:
            data_type = known[0] if known else UTF8
        elif type_name in TYPE_NAMES:
            data_type = TYPE_NAMES[type_name]
        else:
            raise SfoError(f"key '{key}': type '{type_name}' must be one of: utf8, utf8-special, int4")

        # the element's text is the value (an attribute would fight multi-line LICENSE text)
        value = (element.text or "").strip()
        if key in overrides:
            value = overrides[key]

        if data_type == INT4:
            number = parse_int(value)
            if number is None:
                raise SfoError(f"key '{key}' has type int4 but value '{value}' is not an integer")
            params.append((key, INT4, 4, struct.pack("<i", number)))
            continue

        raw_max = element.get("maxlength")
        if raw_max is not None:
            if not raw_max.isdigit() or int(raw_max) <= 0:
                raise SfoError(f"key '{key}' maxlength '{raw_max}' must be a positive integer")
            max_length = int(raw_max)
        elif known:
            max_length = known[1]
        else:
            raise SfoError(f"key '{key}' is not a well-known SFO key; maxlength attribute is required")

        utf8 = value.encode("utf-8")
        if data_type == UTF8_SPECIAL:
            if len(utf8) > max_length:
                raise SfoError(f"key '{key}' value is {len(utf8)} bytes but maxlength is {max_length}")
            payload = utf8
        else:
            if len(utf8) + 1 > max_length:
                raise SfoError(f"key '{key}' value is {len(utf8) + 1} bytes (incl. null) "
                               f"but maxlength is {max_length}")
            payload = utf8 + b"\0"
        params.append((key, data_type, max_length, payload))

    missing = set(overrides) - seen
    if missing:
        raise SfoError(f"{', '.join(sorted(missing))} not in the XML - nothing to override")
    if not params:
        raise SfoError("<paramsfo> contains no <param> entries")
    return params


def build(params):
    entries = sorted(params, key=lambda p: p[0].encode("ascii"))

    key_table = b""
    key_offsets = []
    for key, _, _, _ in entries:
        key_offsets.append(len(key_table))
        key_table += key.encode("ascii") + b"\0"
    key_table += b"\0" * (-len(key_table) % 4)

    key_table_start = HEADER_SIZE + len(entries) * INDEX_ENTRY_SIZE
    data_table_start = key_table_start + len(key_table)

    out = struct.pack("<IIIII", MAGIC, VERSION, key_table_start, data_table_start, len(entries))
    data_offset = 0
    for (key, data_type, max_length, payload), key_offset in zip(entries, key_offsets):
        out += struct.pack("<HHIII", key_offset, data_type, len(payload), max_length, data_offset)
        data_offset += max_length
    out += key_table
    for _, _, max_length, payload in entries:
        out += payload + b"\0" * (max_length - len(payload))
    return out


def main():
    parser = argparse.ArgumentParser(description="Build PARAM.SFO from its XML description.")
    parser.add_argument("xml")
    parser.add_argument("out")
    parser.add_argument("--title", help="replaces TITLE, the name the XMB shows")
    parser.add_argument("--app-ver", help="replaces APP_VER (XX.YY)")
    args = parser.parse_args()

    overrides = {}
    if args.title is not None:
        overrides["TITLE"] = args.title
    if args.app_ver is not None:
        overrides["APP_VER"] = args.app_ver
    try:
        data = build(parse(args.xml, overrides))
    except SfoError as error:
        print(f"{args.xml}: {error}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as f:
        f.write(data)
    title = overrides.get("TITLE") or "(from the XML)"
    print(f"wrote {args.out} ({len(data)} bytes), TITLE {title}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

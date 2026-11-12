#!/usr/bin/env bash
# Package a built yo-player ELF as ONE package for every console - PS3HEN as well as CFW/DEX (since V3.0.0).
#
#   yo-player.ppu.elf --(scetool, NPDRM SELF)--> EBOOT.BIN --(pkg_custom)--> .pkg
#
# build-pkg-fromsource.sh signs with the SDK's make_fself_npdrm: a fake-signed SELF, which CFW and DEX run and
# PS3HEN does not. This one signs the EBOOT as a real NPDRM SELF (free license, the app's content id), as the HEN
# package of v2.0.0 was - and that one package installs and runs on HEN, CFW and DEX alike. On CFW/DEX the app then
# keeps its data in /dev_hdd0/tee-vanced (it copies an older data folder over once).
#
# It runs on the machine that has the tools (Windows, Git Bash), and needs three of them, named by environment
# variable - none of them is part of this repository:
#   SCETOOL_DIR   the folder of scetool.exe, with the keys scetool needs in its data/ folder. Never commit keys,
#                 never paste them anywhere: this script only passes the folder on.
#   PKG_CUSTOM    pkg_custom.exe (makes the .pkg from the staged content folder)
#   PPU_READELF   ppu-lv2-readelf.exe from the Cell SDK (to check the signed EBOOT against the ELF), optional
#
# usage: tools/package-universal.sh <elf> <out-dir> [<pkg-name-without-extension>] [<xmb title>]
#        the name and title default to the version in apps/yo-player/include/app-paths.h
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:?usage: package-universal.sh <elf> <out-dir> [pkg-name] [title]}"
OUT="${2:?usage: package-universal.sh <elf> <out-dir> [pkg-name] [title]}"
: "${SCETOOL_DIR:?set SCETOOL_DIR to the folder of scetool.exe (with its data/ keys)}"
: "${PKG_CUSTOM:?set PKG_CUSTOM to pkg_custom.exe}"
CONTENT_ID="HB0001-TEEV00001_00-TEEVANCEDPS30001"
PYTHON="${PYTHON:-$(command -v python || command -v python3)}"   # Windows: python3 may be the Store stub

VERSION="$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' "$ROOT/apps/yo-player/include/app-paths.h")"   # V3.0.0 Test 12 Fix
PKG_NAME="${3:-TEE-Vanced-PS3-$(printf '%s' "$VERSION" | tr ' ' '-')}"
PKG_TITLE="${4:-TEE Vanced PS3 $VERSION}"

[ -f "$ELF" ] || { echo "no ELF at $ELF" >&2; exit 1; }
[ -e "$OUT" ] && { echo "$OUT exists - pick a new folder, an old package is never overwritten" >&2; exit 1; }
mkdir -p "$OUT/content/USRDIR"

echo "==> stage: PARAM.SFO ($PKG_TITLE), the XMB art, the package config"
"$PYTHON" -X utf8 -I "$ROOT/pkg/make-param-sfo.py" --title "$PKG_TITLE" "$ROOT/apps/yo-player/PARAM.SFO.xml" "$OUT/content/PARAM.SFO"
cp "$ROOT/branding/ICON0.PNG" "$ROOT/branding/PIC1.PNG" "$OUT/content/"
cp "$ROOT/apps/yo-player/yo-player.conf" "$OUT/package.conf"
cp "$ELF" "$OUT/yo-player.ppu.elf"

echo "==> sign: NPDRM SELF (scetool)"
OUT_WIN="$(cd "$OUT" && pwd -W 2>/dev/null || pwd)"
( cd "$SCETOOL_DIR" && ./scetool.exe -0 SELF -5 NPDRM -1 TRUE -s FALSE -7 TRUE -2 1 \
     -6 0003004000000000 -A 0001000000000000 -3 1010000001000003 -4 01000002 \
     --self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002 \
     --self-cap-flags 00000000000000000000000000000000000000000000003B0000000100042000 \
     --np-license-type=FREE --np-app-type=EXEC --np-real-fname=EBOOT.BIN --np-content-id="$CONTENT_ID" \
     -e "$OUT_WIN/yo-player.ppu.elf" "$OUT_WIN/content/USRDIR/EBOOT.BIN" | tail -1 )

echo "==> package (pkg_custom)"
# pkg_custom waits for a key at its end: an empty stdin answers it. Its manifest asks for elevation, which it does not
# need: RunAsInvoker starts it as the user.
( cd "$OUT" && __COMPAT_LAYER=RunAsInvoker "$PKG_CUSTOM" -c "$CONTENT_ID" content "$PKG_NAME.pkg" < /dev/null      | grep -E "Package size|Content ID" || true )
[ -f "$OUT/$PKG_NAME.pkg" ] || { echo "no package was written" >&2; exit 1; }

if [ -n "${PPU_READELF:-}" ]; then
   echo "==> check: the signed EBOOT decrypts to the ELF's loaded segments"
   ( cd "$SCETOOL_DIR" && ./scetool.exe -d "$OUT_WIN/content/USRDIR/EBOOT.BIN" "$OUT_WIN/eboot-decrypted.elf" | tail -1 )
   "$PYTHON" -X utf8 -I - "$OUT" "$PPU_READELF" <<'EOF'
import subprocess, sys
out, readelf = sys.argv[1], sys.argv[2]
heads = subprocess.run([readelf, '-lW', out + '/yo-player.ppu.elf'], capture_output=True, text=True).stdout
a = open(out + '/yo-player.ppu.elf', 'rb').read(); b = open(out + '/eboot-decrypted.elf', 'rb').read()
rows = [l.split() for l in heads.splitlines()]
ok = all(a[int(f[1], 16):int(f[1], 16) + int(f[4], 16)] == b[int(f[1], 16):int(f[1], 16) + int(f[4], 16)]
         for f in rows if f and f[0] in ('LOAD', 'TLS', 'LOOS+1', 'LOOS+2'))
print('all loaded segments identical' if ok else 'SEGMENTS DIFFER')
sys.exit(0 if ok else 1)
EOF
   rm -f "$OUT/eboot-decrypted.elf"
fi

echo "==> $OUT/$PKG_NAME.pkg"
sha256sum "$OUT/$PKG_NAME.pkg" | cut -c1-64

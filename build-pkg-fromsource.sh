#!/usr/bin/env bash
# Package a from-source yo-player build, using the Cell SDK's own tools on the dev VM.
#
#   yo-player.ppu.elf --(make_fself_npdrm)--> EBOOT.BIN --(make_package_npdrm)--> .pkg
#
# This is the chain mohasi's own yo-player.conf is written for, and it builds a complete, correctly sized
# package from scratch. The earlier approach - swapping files inside his released PKG - was only ever
# necessary back when we could not compile the sources; it forced the payload to stay under 411072 bytes
# (the signed header states the size and cannot be regenerated), which is why the icon had to be
# recompressed and the app briefly built for size. None of that applies here: the package can be any size,
# and the icon ships untouched.
#
# Everything on this side is plain bash 3.2 + python3 + ssh, so it runs from macOS as well as from Linux; only
# the two SDK tools run on the VM.
#
# usage: build-pkg-fromsource.sh [<pkg-name-without-extension>] [<xmb title>]
#        both default to the version in app-paths.h: TEE-Vanced-PS3-v0.12-beta / "TEE Vanced PS3 V0.12 Beta"
set -euo pipefail


# Machine-specific settings live in build.env next to this script, which is git-ignored - the build VM's
# address, the key that reaches it and where packages land are one person's setup, not part of the project.
# Copy build.env.example to build.env and fill it in. tools/build-env.sh reads it.
ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$ROOT/tools/build-env.sh"
PKG_DIR="${PS3_PKG_DIR:-$ROOT/pkg}"
OUT_DIR="$PKG_DIR/out"
SDK_BIN="$VM_SDK\\host-win32\\bin"
VM_ELF='C:\ps3-dev\apps\yo-player\bin\Release\yo-player.ppu.elf'
STAGE='C:\pkgbuild'
CONTENT_ID="HB0001-YOPLAYER1_00-YOPLAYERHB000001"

VERSION="$(app_version)"                                       # V0.12 Beta
VERSION_SLUG="$(printf '%s' "$VERSION" | tr 'A-Z ' 'a-z-')"    # v0.12-beta
PKG_NAME="${1:-TEE-Vanced-PS3-${VERSION_SLUG:-fromsource}}"
PKG_TITLE="${2:-TEE Vanced PS3${VERSION:+ $VERSION}}"
export PKG_TITLE

mkdir -p "$OUT_DIR"

# PARAM.SFO is built from the app's own PARAM.SFO.xml rather than patched into one extracted from mohasi's
# released package, which only ever existed on the machine it was extracted on. For the same title the two
# give the same file (see pkg/make-param-sfo.py).
# (This fork's copy, not the clone's: mohasi's has APP_VER 01.00 where every released package has 01.74.)
SFO_XML="$ROOT/apps/yo-player/PARAM.SFO.xml"
[ -f "$SFO_XML" ] || { echo "missing $SFO_XML - this script runs from a checkout of TEE-Vanced-PS3" >&2; exit 1; }
echo "==> XMB title: $PKG_TITLE"
python3 "$ROOT/pkg/make-param-sfo.py" "$SFO_XML" "$OUT_DIR/PARAM.SFO" --title "$PKG_TITLE"

# Our own XMB artwork (tools/make-branding.py renders both). ICON0 is the tile in the game column; PIC1 is
# the full-screen background the XMB shows while that tile is selected - mohasi's package had no PIC1 at all.
BRANDING="$ROOT/branding"
for art in ICON0.PNG PIC1.PNG; do
  [ -f "$BRANDING/$art" ] || { echo "missing $BRANDING/$art - run tools/make-branding.py first" >&2; exit 1; }
done

echo "==> stage the package contents on the VM"
vm_ssh "rmdir /S /Q $STAGE 2>nul & mkdir $STAGE\\content\\USRDIR" || true
vm_scp "$OUT_DIR/PARAM.SFO"          "$VM_HOST:C:/pkgbuild/content/PARAM.SFO" >/dev/null
vm_scp "$BRANDING/ICON0.PNG"         "$VM_HOST:C:/pkgbuild/content/ICON0.PNG" >/dev/null
vm_scp "$BRANDING/PIC1.PNG"          "$VM_HOST:C:/pkgbuild/content/PIC1.PNG" >/dev/null
vm_scp "$ROOT/ps3-dev/apps/yo-player/yo-player.conf" "$VM_HOST:C:/pkgbuild/package.conf" >/dev/null

echo "==> sign the ELF (make_fself_npdrm)"
vm_ssh "cd /d $STAGE && $SDK_BIN\\make_fself_npdrm.exe -c $VM_ELF content\\USRDIR\\EBOOT.BIN"

echo "==> build the package (make_package_npdrm)"
vm_ssh "cd /d $STAGE && $SDK_BIN\\make_package_npdrm.exe package.conf content" | tail -4

echo "==> fetch it"
vm_scp "$VM_HOST:C:/pkgbuild/$CONTENT_ID.pkg" "$OUT_DIR/$PKG_NAME.pkg" >/dev/null

echo
echo "Done: $OUT_DIR/$PKG_NAME.pkg"
ls -la "$OUT_DIR/$PKG_NAME.pkg"
sha256_of "$OUT_DIR/$PKG_NAME.pkg"

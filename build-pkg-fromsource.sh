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
# usage: build-pkg-fromsource.sh <pkg-name-without-extension> <xmb title>
set -euo pipefail


# Machine-specific settings live in build.env next to this script, which is git-ignored - the build VM's
# address, the key that reaches it and where packages land are one person's setup, not part of the project.
# Copy build.env.example to build.env and fill it in.
ROOT="$(cd "$(dirname "$0")" && pwd)"
[ -f "$ROOT/build.env" ] && . "$ROOT/build.env"
[ -f "$ROOT/../build.env" ] && . "$ROOT/../build.env"
VM_HOST="${PS3_BUILD_VM:-builder@ps3-build-vm}"
SSH_KEY="${PS3_BUILD_KEY:-$HOME/.ssh/id_ed25519}"
PKG_DIR="${PS3_PKG_DIR:-$ROOT/pkg}"
OUT_DIR="$PKG_DIR/out"
SSH_OPTS="-i $SSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
SDK_BIN='C:\usr\local\cell\host-win32\bin'
VM_ELF='C:\ps3-dev\apps\yo-player\bin\Release\yo-player.ppu.elf'
STAGE='C:\pkgbuild'
CONTENT_ID="HB0001-YOPLAYER1_00-YOPLAYERHB000001"

PKG_NAME="${1:-TEE-Yo-Player-fromsource}"
PKG_TITLE="${2:-TEE Yo Player}"
export PKG_TITLE

mkdir -p "$OUT_DIR"

echo "==> XMB title: $PKG_TITLE"
python3 "$PKG_DIR/patch-param-title.py" "$PKG_DIR/extracted/PARAM.SFO" "$OUT_DIR/PARAM.SFO" "$PKG_TITLE"

# Our own XMB artwork (tools/make-branding.py renders both). ICON0 is the tile in the game column; PIC1 is
# the full-screen background the XMB shows while that tile is selected - mohasi's package had no PIC1 at all.
BRANDING="$ROOT/branding"
for art in ICON0.PNG PIC1.PNG; do
  [ -f "$BRANDING/$art" ] || { echo "missing $BRANDING/$art - run tools/make-branding.py first" >&2; exit 1; }
done

echo "==> stage the package contents on the VM"
ssh $SSH_OPTS "$VM_HOST" "rmdir /S /Q $STAGE 2>nul & mkdir $STAGE\\content\\USRDIR" || true
scp $SSH_OPTS "$OUT_DIR/PARAM.SFO"          "$VM_HOST:C:/pkgbuild/content/PARAM.SFO" >/dev/null
scp $SSH_OPTS "$BRANDING/ICON0.PNG"         "$VM_HOST:C:/pkgbuild/content/ICON0.PNG" >/dev/null
scp $SSH_OPTS "$BRANDING/PIC1.PNG"          "$VM_HOST:C:/pkgbuild/content/PIC1.PNG" >/dev/null
scp $SSH_OPTS "$ROOT/ps3-dev/apps/yo-player/yo-player.conf" "$VM_HOST:C:/pkgbuild/package.conf" >/dev/null

echo "==> sign the ELF (make_fself_npdrm)"
ssh $SSH_OPTS "$VM_HOST" "cd /d $STAGE && $SDK_BIN\\make_fself_npdrm.exe -c $VM_ELF content\\USRDIR\\EBOOT.BIN"

echo "==> build the package (make_package_npdrm)"
ssh $SSH_OPTS "$VM_HOST" "cd /d $STAGE && $SDK_BIN\\make_package_npdrm.exe package.conf content" | tail -4

echo "==> fetch it"
scp $SSH_OPTS "$VM_HOST:C:/pkgbuild/$CONTENT_ID.pkg" "$OUT_DIR/$PKG_NAME.pkg" >/dev/null

echo
echo "Done: $OUT_DIR/$PKG_NAME.pkg"
ls -la "$OUT_DIR/$PKG_NAME.pkg"
sha256sum "$OUT_DIR/$PKG_NAME.pkg"

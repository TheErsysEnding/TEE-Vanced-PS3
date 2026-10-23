#!/usr/bin/env bash
# Build TEE Vanced PS3 from this checkout, start to finish: fork files -> upstream clone -> build VM -> .elf
# -> .pkg. The machine this runs on (a Mac, or the Linux PC) only needs bash, python3, git and ssh; the
# compiler and the two packaging tools are the Cell SDK's and run on the Windows VM named in build.env.
#
#   ./build.sh            build and package
#   ./build.sh --no-pkg   build and check the .elf, skip the package
#
# Each step stops the run on failure. The checks after the compile are not decoration: a build here once
# reported success while linking a stale object, and a package that did not contain the change was shipped
# and tested on that basis (see genbuild.py). So the .elf is fetched back and must contain both the version
# string and this run's build stamp before anything is packaged.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$ROOT/tools/build-env.sh"
OUT_DIR="${PS3_PKG_DIR:-$ROOT/pkg}/out"
mkdir -p "$OUT_DIR"

make_pkg=1
case "${1:-}" in
   "") ;;
   --no-pkg) make_pkg=0 ;;
   *) echo "usage: build.sh [--no-pkg]" >&2; exit 2 ;;
esac

echo "==> upstream clone"
"$ROOT/tools/fetch-upstream.sh"

echo "==> this fork's files into the clone"
"$ROOT/tools/sync-work.sh" --local

echo "==> build-ps3.bat"
python3 "$ROOT/genbuild.py"

echo "==> sources onto the VM"
"$ROOT/tools/sync-work.sh"

# <commit count>-<commit>[-dirty], from this repository: the VM has no git, and this checkout is the source
# that matters. Only changes to what gets compiled make it dirty.
if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
   STAMP="$(git -C "$ROOT" rev-list --count HEAD)-$(git -C "$ROOT" rev-parse --short HEAD)"
   if [ -n "$(git -C "$ROOT" status --porcelain -- apps libs work 2>/dev/null)" ]; then
      STAMP="$STAMP-dirty"
   fi
else
   STAMP="0-unknown"
fi
VERSION="$(app_version)"

echo "==> compiling on $VM_HOST (stamp $STAMP)"
LOG="$OUT_DIR/build.log"
# The batch's own exit code does not always survive cmd.exe and ssh, so its last line decides: ALL-OK or not.
vm_ssh "cd /d C:\\ps3-dev && build-ps3.bat $STAMP" > "$LOG" 2>&1 || true
tr -d '\r' < "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"
if ! grep -qx "ALL-OK" "$LOG"; then
   echo "--- last lines of $LOG ---" >&2
   tail -n 30 "$LOG" >&2
   echo >&2
   echo "BUILD FAILED - full output in $LOG" >&2
   exit 1
fi
# -Werror is left off (genbuild.py), so warnings do not stop the build - but they are worth seeing.
warnings="$(grep -c -i "warning:" "$LOG" || true)"
if [ "${warnings:-0}" != 0 ]; then
   echo "   $warnings compiler warning(s), first ones:"
   grep -i "warning:" "$LOG" | head -n 10 | sed 's/^/      /' || true
fi

echo "==> fetching the .elf"
ELF="$OUT_DIR/yo-player.ppu.elf"
rm -f "$ELF"
vm_scp "$VM_HOST:C:/ps3-dev/apps/yo-player/bin/Release/yo-player.ppu.elf" "$ELF" >/dev/null

for needle in "$VERSION" "$STAMP"; do
   if [ -z "$needle" ] || ! LC_ALL=C grep -q -a -F -- "$needle" "$ELF"; then
      echo "the .elf does not contain \"$needle\" - it is not the build that was just asked for" >&2
      exit 1
   fi
done
echo "   $ELF: contains \"$VERSION\" and \"$STAMP\""

[ "$make_pkg" = 1 ] || exit 0

echo "==> packaging"
"$ROOT/build-pkg-fromsource.sh"

#!/usr/bin/env bash
# Mirror work/ into the upstream clone and then onto the build VM.
#
# work/ is a FLAT working copy; the clone (and the VM) use mohasi's real directory layout, so every file has
# to be matched by name. Doing that by hand, one scp per changed file, is how three files in the clone drifted
# back to their August versions without anyone noticing - including youtube.c, which still had the pre-VISIONOS
# client, and a play.h with no playVideoFromList declaration. Nothing broke only because those files happened
# never to be uploaded again; the next upload from the clone would have silently undone a shipped fix.
#
# This copies everything, every time, and fails loudly on a file it cannot place.
#
#   tools/sync-work.sh            mirror work/ -> clone -> VM
#   tools/sync-work.sh --local    mirror work/ -> clone only (no VM)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/ps3-dev/apps/yo-player"

# See build.env.example - the build machine is one person's setup, not part of the project.
[ -f "$ROOT/build.env" ] && . "$ROOT/build.env"
VM_HOST="${PS3_BUILD_VM:-builder@ps3-build-vm}"
SSH_KEY="${PS3_BUILD_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS="-i $SSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# Files that live in work/ only for reference - they belong to the shared libraries, not to the app, and
# copying them into apps/yo-player would create a second, diverging copy of a library source.
IS_REFERENCE="console-glyphs.c console-glyphs.h decode-h264.c decode-h264.h live-source.c video-player.c dbg.h http.h"

copied=0
skipped=0
for src in "$ROOT"/work/*.c "$ROOT"/work/*.h; do
   base="$(basename "$src")"
   case " $IS_REFERENCE " in *" $base "*) skipped=$((skipped + 1)); continue ;; esac

   dst="$(find "$APP" -name "$base" -print -quit)"
   if [ -z "$dst" ]; then
      echo "no home in the clone for $base - add it under apps/yo-player/src or include first" >&2
      exit 1
   fi
   if ! cmp -s "$src" "$dst"; then
      cp "$src" "$dst"
      echo "  updated ${dst#$ROOT/}"
      copied=$((copied + 1))
   fi
done
echo "clone: $copied file(s) updated, $skipped library reference(s) left alone"

[ "${1:-}" = "--local" ] && exit 0

# The VM gets the whole app tree, so a file that was only ever fixed in the clone cannot stay behind there
# either. scp -r on the two directories is cheaper than working out what changed.
echo "==> uploading the app tree to the VM"
scp $SSH_OPTS -r "$APP/src"     "$VM_HOST:C:/ps3-dev/apps/yo-player/" >/dev/null
scp $SSH_OPTS -r "$APP/include" "$VM_HOST:C:/ps3-dev/apps/yo-player/" >/dev/null
scp $SSH_OPTS "$ROOT/build-ps3.bat" "$VM_HOST:C:/ps3-dev/build-ps3.bat" >/dev/null
echo "VM: app tree + build-ps3.bat in sync"

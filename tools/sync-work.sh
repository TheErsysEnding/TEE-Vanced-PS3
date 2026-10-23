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
# Without a work/ directory - a fresh clone of this repository, which carries the fork's files in mohasi's own
# layout under apps/ and libs/ - those two trees are the source instead, and they are copied path for path.
#
#   tools/sync-work.sh            mirror work/ (or apps/ + libs/) -> clone -> VM
#   tools/sync-work.sh --local    mirror work/ (or apps/ + libs/) -> clone only (no VM)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/ps3-dev/apps/yo-player"

# See build.env.example - the build machine is one person's setup, not part of the project.
. "$ROOT/tools/build-env.sh"

if [ ! -d "$APP" ]; then
   echo "no upstream clone at ${APP%/apps/yo-player} - run tools/fetch-upstream.sh first" >&2
   exit 1
fi

# Files that live in work/ only for reference - they belong to the shared libraries, not to the app, and
# copying them into apps/yo-player would create a second, diverging copy of a library source.
IS_REFERENCE="console-glyphs.c console-glyphs.h decode-h264.c decode-h264.h live-source.c video-player.c dbg.h http.h"

copied=0
skipped=0
if [ -d "$ROOT/work" ]; then
   for src in "$ROOT"/work/*.c "$ROOT"/work/*.h; do
      [ -e "$src" ] || continue          # an unmatched glob stays literal
      base="$(basename "$src")"
      case " $IS_REFERENCE " in *" $base "*) skipped=$((skipped + 1)); continue ;; esac

      dst="$(find "$APP" -name "$base" -print -quit)"
      if [ -z "$dst" ]; then
         echo "no home in the clone for $base - add it under apps/yo-player/src or include first" >&2
         exit 1
      fi
      if ! cmp -s "$src" "$dst"; then
         cp "$src" "$dst"
         echo "  updated ${dst#"$ROOT"/}"
         copied=$((copied + 1))
      fi
   done
   echo "clone: $copied file(s) updated, $skipped library reference(s) left alone"
else
   # apps/ and libs/ already use the clone's layout, so a file's path IS its home - nothing to look up, and a
   # file the fork adds (ps-button.c, sidebar.c ...) lands next to its neighbours. genbuild.py compiles any .c
   # it finds there, listed in mohasi's .vcxproj or not.
   for tree in apps libs; do
      [ -d "$ROOT/$tree" ] || continue
      while IFS= read -r rel; do
         [ -n "$rel" ] || continue
         src="$ROOT/$rel"
         dst="$ROOT/ps3-dev/$rel"
         if ! cmp -s "$src" "$dst" 2>/dev/null; then
            mkdir -p "$(dirname "$dst")"
            cp "$src" "$dst"
            echo "  updated ps3-dev/$rel"
            copied=$((copied + 1))
         fi
      done <<EOF
$(cd "$ROOT" && find "$tree" -type f ! -name '.DS_Store' | sort)
EOF
   done
   echo "clone: $copied file(s) updated from apps/ and libs/"
fi

[ "${1:-}" = "--local" ] && exit 0

if [ ! -f "$ROOT/build-ps3.bat" ]; then
   echo "no build-ps3.bat - run python3 genbuild.py first" >&2
   exit 1
fi

# The VM gets the whole app tree, so a file that was only ever fixed in the clone cannot stay behind there
# either. scp -r on the two directories is cheaper than working out what changed.
echo "==> uploading the app tree to the VM"
vm_scp -r "$APP/src"     "$VM_HOST:C:/ps3-dev/apps/yo-player/" >/dev/null
vm_scp -r "$APP/include" "$VM_HOST:C:/ps3-dev/apps/yo-player/" >/dev/null

# The same for every library directory this fork changes a file in (libs/simple-lib-app/src for pad.c): the
# app tree alone never carries pad.c, so without this the VM keeps whatever copy it was first given.
if [ -d "$ROOT/libs" ]; then
   for dir in "$ROOT"/libs/*/*/; do
      [ -d "$dir" ] || continue
      rel="${dir#"$ROOT"/}"; rel="${rel%/}"            # libs/simple-lib-app/src
      parent="${rel%/*}"                             # libs/simple-lib-app
      vm_scp -r "$ROOT/ps3-dev/$rel" "$VM_HOST:C:/ps3-dev/$parent/" >/dev/null
      echo "  uploaded ps3-dev/$rel"
   done
fi

vm_scp "$ROOT/build-ps3.bat" "$VM_HOST:C:/ps3-dev/build-ps3.bat" >/dev/null
echo "VM: app tree + build-ps3.bat in sync"

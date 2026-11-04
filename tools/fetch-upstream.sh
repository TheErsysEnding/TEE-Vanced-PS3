#!/usr/bin/env bash
# Put mohasi's ps3-dev tree next to this repository, at the commit this fork was built on.
#
# This repository holds only the files the fork changes; everything else - the libraries, the shaders, the
# project files genbuild.py reads - comes from upstream. A plain `git clone` gives whatever upstream is today,
# and his tree has moved on since the fork was written (see UPSTREAM_REV in build-env.sh). So this clones, then
# checks out the pinned commit, and refuses to touch a clone that sits on another commit with edits in it.
#
#   tools/fetch-upstream.sh            clone ps3-dev/ if missing, check it is on the pinned commit
#   tools/fetch-upstream.sh --reset    put an existing clone back on the pinned commit, discarding edits in it
#
# The fork's own files are laid over the clone by tools/sync-work.sh, not here.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/build-env.sh"
CLONE="$ROOT/ps3-dev"

if [ ! -d "$CLONE/.git" ]; then
   if [ -e "$CLONE" ]; then
      echo "$CLONE exists but is not a git clone - move it aside first" >&2
      exit 1
   fi
   echo "==> cloning $UPSTREAM_URL"
   # Blobs are fetched only for the commit that gets checked out: the full history is ~230 MB of files this
   # build never reads (tools, plugins, other apps at every revision).
   git clone --filter=blob:none --no-checkout "$UPSTREAM_URL" "$CLONE"
   git -C "$CLONE" checkout --quiet --detach "$UPSTREAM_REV"
   echo "ps3-dev/ at $(git -C "$CLONE" log -1 --format='%h %s')"
   exit 0
fi

want="$(git -C "$CLONE" rev-parse --verify --quiet "$UPSTREAM_REV^{commit}" || true)"
if [ -z "$want" ]; then
   echo "==> fetching $UPSTREAM_REV"
   git -C "$CLONE" fetch --quiet origin
   want="$(git -C "$CLONE" rev-parse --verify "$UPSTREAM_REV^{commit}")"
fi
have="$(git -C "$CLONE" rev-parse HEAD)"

if [ "$have" = "$want" ]; then
   echo "ps3-dev/ is on the pinned commit $(git -C "$CLONE" log -1 --format='%h %s')"
   exit 0
fi

if [ "${1:-}" != "--reset" ]; then
   echo "ps3-dev/ is on $(git -C "$CLONE" log -1 --format='%h %ad %s' --date=short)" >&2
   echo "        expected $(git -C "$CLONE" log -1 --format='%h %ad %s' --date=short "$want")" >&2
   echo >&2
   echo "Building on another upstream commit mixes two versions of mohasi's libraries. Run" >&2
   echo "   tools/fetch-upstream.sh --reset" >&2
   echo "to move it (edits inside ps3-dev/ are discarded - this fork's files are copied back by sync-work.sh)," >&2
   echo "or set PS3_UPSTREAM_REV in build.env if the other commit is intended." >&2
   exit 1
fi

echo "==> moving ps3-dev/ to $UPSTREAM_REV"
git -C "$CLONE" checkout --quiet --force --detach "$want"
echo "ps3-dev/ at $(git -C "$CLONE" log -1 --format='%h %s')"

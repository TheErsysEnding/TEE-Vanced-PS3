# Sourced by every script that talks to the build machine - never run on its own.
#
# One place for the machine-specific settings (build.env), the ssh options and the few commands that differ
# between macOS and Linux, so the scripts stay runnable from either. macOS still ships bash 3.2, so nothing in
# here or in the scripts that source it may use bash 4 features (associative arrays, ${var,,}, mapfile).
#
# Expects ROOT to be set to the repository root by the caller.

# Machine-specific settings live in build.env next to the scripts, which is git-ignored - the build VM's
# address, the key that reaches it and where packages land are one person's setup, not part of the project.
# Copy build.env.example to build.env and fill it in.
[ -f "$ROOT/build.env" ] && . "$ROOT/build.env"
[ -f "$ROOT/../build.env" ] && . "$ROOT/../build.env"

VM_HOST="${PS3_BUILD_VM:-builder@ps3-build-vm}"
SSH_KEY="${PS3_BUILD_KEY:-$HOME/.ssh/id_ed25519}"
VM_PORT="${PS3_BUILD_PORT:-22}"
VM_JUMP="${PS3_BUILD_JUMP:-}"

# -o Port= rather than -p/-P: ssh and scp spell the port flag differently, the -o form is the same for both.
SSH_OPTS="-i $SSH_KEY -o Port=$VM_PORT -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
# A VM that is only reachable through another machine (a Windows VM on a NAT network behind the Linux PC,
# say) goes through it as a jump host, so it does not need a port forward of its own.
[ -n "$VM_JUMP" ] && SSH_OPTS="$SSH_OPTS -o ProxyJump=$VM_JUMP"

# Where the Cell SDK sits on the build machine - the location its own installer uses. genbuild.py and
# build-pkg-fromsource.sh name the same directory; it is not a setting because nothing has ever needed it moved.
VM_SDK='C:\usr\local\cell'

# The upstream commit this fork's files were written against: eb4be76, the head of mohasi's tree when v0.11
# was built (2026-08-31, the last commit before 2026-09-06). His tree has moved on since - Opus, DTLS, a new
# pad.c with BD-remote mapping - and laying this fork's files over a newer checkout would quietly undo some of
# his changes and mix two versions of the same library. Moving to a newer upstream is a deliberate step:
# change this, rebuild, test on the console.
UPSTREAM_URL="${PS3_UPSTREAM_URL:-https://github.com/mohasi/ps3-dev}"
UPSTREAM_REV="${PS3_UPSTREAM_REV:-eb4be76fd89866e4dfbcfadc0b2eced3337c014b}"

# The version the app shows ("V0.12 Beta"), read from the clone after sync-work.sh put this fork's
# app-paths.h there - that is the copy that gets compiled, whichever source (work/ or apps/) it came from.
app_version() {
   sed -n 's/^#define APP_VERSION "\(.*\)".*/\1/p' "$ROOT/ps3-dev/apps/yo-player/include/app-paths.h" 2>/dev/null | head -n 1
}

# sha256sum is GNU coreutils; macOS has shasum instead.
sha256_of() {
   if command -v sha256sum >/dev/null 2>&1; then
      sha256sum "$1"
   else
      shasum -a 256 "$1"
   fi
}

vm_ssh() {
   # shellcheck disable=SC2086
   ssh $SSH_OPTS "$VM_HOST" "$@"
}

vm_scp() {
   # shellcheck disable=SC2086
   scp $SSH_OPTS "$@"
}

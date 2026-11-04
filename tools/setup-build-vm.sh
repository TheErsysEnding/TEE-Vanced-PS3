#!/usr/bin/env bash
# Check a build VM from end to end, and give a fresh one the source tree it builds from.
#
# The build VM is a Windows machine with Sony's Cell SDK at C:\usr\local\cell and an OpenSSH server - on a
# Mac, a UTM (or Parallels / VMware Fusion) VM; see docs/BUILD-MACOS.md. Nothing in the SDK runs on macOS, so
# this is the one part of the build that cannot move to the Mac. What this script checks, in order:
#
#   1. the tools on this side (ssh, scp, git, python3) and build.env
#   2. ssh reaches the VM, with the key, without a password prompt
#   3. the VM's ssh shell is cmd.exe - every remote command here and in the build is cmd syntax
#   4. the SDK files the build uses are there
#   5. every SDK program the build runs actually starts (a fresh Windows can lack the runtime DLLs they need)
#   6. ppu-lv2-gcc compiles a test program, and links it - the proof the toolchain works on this VM
#   7. C:\ps3-dev holds mohasi's libraries and the app; if it does not, they are uploaded from ps3-dev/
#
#   tools/setup-build-vm.sh               check, and upload the source tree only if the VM has none
#   tools/setup-build-vm.sh --push-tree   check, and upload the source tree in any case (after moving
#                                         UPSTREAM_REV, or to replace a tree of unknown origin)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/build-env.sh"

push_tree=0
case "${1:-}" in
   "") ;;
   --push-tree) push_tree=1 ;;
   *) echo "usage: setup-build-vm.sh [--push-tree]" >&2; exit 2 ;;
esac

# genbuild.py's LIBS plus the app: everything build-ps3.bat compiles or includes.
TREE="libs/simple-lib-core libs/simple-lib-https libs/simple-lib-av libs/simple-lib-app apps/yo-player"

fail() {
   echo "  FAIL  $1" >&2
   shift
   for line in "$@"; do echo "        $line" >&2; done
   exit 1
}
ok() { echo "  ok    $1"; }

# Runs a cmd.exe command line on the VM and prints its output, CRs removed. Its stdin is at end-of-file, so a
# program that waits for input cannot hang the check.
remote() {
   vm_ssh "$1" </dev/null 2>&1 | tr -d '\r'
}

# Whether a path exists on the VM. Paths here never contain spaces, so they go unquoted - a quoted path at the
# start of a cmd /c line is subject to cmd's quote-stripping rules.
remote_exists() {
   [ "$(remote "if exist $1 (echo yes) else (echo no)")" = "yes" ]
}

echo "==> this machine"
for tool in ssh scp git python3; do
   command -v "$tool" >/dev/null 2>&1 || fail "$tool not found" \
      "macOS: xcode-select --install   (brings git and python3; ssh and scp are built in)"
done
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 6) else 1)' \
   || fail "python3 is older than 3.6" "install a current one (xcode-select --install, or python.org)"
ok "ssh, scp, git, python3 ($(python3 -c 'import platform; print(platform.python_version())'))"

[ -f "$ROOT/build.env" ] || [ -f "$ROOT/../build.env" ] \
   || fail "no build.env" "cp build.env.example build.env, then fill in PS3_BUILD_VM and PS3_BUILD_KEY"
[ -f "$SSH_KEY" ] || fail "ssh key $SSH_KEY does not exist" \
   "ssh-keygen -t ed25519 -f $SSH_KEY   and put the .pub on the VM (docs/BUILD-MACOS.md, step 4)"
ok "build.env -> $VM_HOST (port $VM_PORT${VM_JUMP:+, via $VM_JUMP}), key $SSH_KEY"

echo "==> ssh to the VM"
answer="$(remote "echo reachable" || true)"
case "$answer" in
   *reachable*) ok "logged in without a password" ;;
   *) fail "ssh to $VM_HOST did not work:" "$answer" \
         "Is the VM running, OpenSSH Server installed and started (docs/BUILD-MACOS.md, step 3)?" \
         "Is the public key in C:\\ProgramData\\ssh\\administrators_authorized_keys? An admin account's" \
         "~/.ssh/authorized_keys is ignored by Windows' sshd - the most common reason for a password prompt." ;;
esac

# cmd.exe expands %OS% to Windows_NT; PowerShell echoes the text back unchanged.
case "$(remote 'echo %OS%')" in
   *Windows_NT*) ok "remote shell is cmd.exe" ;;
   *) fail "the VM's ssh shell is not cmd.exe" \
         "Every remote command is cmd syntax. Remove the DefaultShell value under" \
         "HKLM\\SOFTWARE\\OpenSSH (it is PowerShell when set) and restart the sshd service." ;;
esac

echo "==> Cell SDK at $VM_SDK"
missing=""
for rel in \
   'host-win32\ppu\bin\ppu-lv2-gcc.exe' \
   'host-win32\ppu\bin\ppu-lv2-ar.exe' \
   'host-win32\ppu\bin\ppu-lv2-objcopy.exe' \
   'host-win32\Cg\bin\sce-cgc.exe' \
   'host-win32\bin\make_fself_npdrm.exe' \
   'host-win32\bin\make_package_npdrm.exe' \
   'target\ppu\include\cell' \
   'target\common\include' \
   'target\ppu\lib\libsysutil_stub.a' \
   'target\ppu\lib\libgcm_cmd.a'; do
   remote_exists "$VM_SDK\\$rel" || missing="$missing $rel"
done
if [ -n "$missing" ]; then
   # shellcheck disable=SC2086   # one line per missing path; none contains a space
   fail "the SDK is missing or incomplete; not found under $VM_SDK:" $missing \
      "Copy the whole cell directory from the machine you built on before (docs/BUILD-MACOS.md, step 5)."
fi
ok "every file the build uses is there"

# The SDK installer sets SCE_PS3_ROOT system-wide; a cell directory copied over by hand does not get it.
# build-ps3.bat passes every include and library path explicitly, but the toolchain's own helpers look the
# variable up, so it is set the way the installer would have set it.
case "$(remote 'echo %SCE_PS3_ROOT%')" in
   *%SCE_PS3_ROOT%*|"")
      fail "SCE_PS3_ROOT is not set on the VM" \
         "In an administrator cmd on the VM:  setx /M SCE_PS3_ROOT $VM_SDK" \
         "then restart the ssh server (PowerShell as admin: Restart-Service sshd) and run this again." ;;
   *) ok "SCE_PS3_ROOT=$(remote 'echo %SCE_PS3_ROOT%')" ;;
esac

# A program that cannot load its DLLs exits with STATUS_DLL_NOT_FOUND (-1073741515) or fails its side-by-side
# activation (14001), and says so only on a desktop nobody is looking at. Run each one once and read the code.
# cmd /v:on, so !errorlevel! is read after the program ran rather than when the line was parsed.
for rel in \
   'host-win32\ppu\bin\ppu-lv2-gcc.exe --version' \
   'host-win32\Cg\bin\sce-cgc.exe -v' \
   'host-win32\bin\make_fself_npdrm.exe' \
   'host-win32\bin\make_package_npdrm.exe'; do
   out="$(remote "cmd /v:on /c \"$VM_SDK\\$rel >%TEMP%\\sdkcheck.txt 2>&1 & echo rc=!errorlevel!\"" || true)"
   rc="$(printf '%s\n' "$out" | sed -n 's/.*rc=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | tail -n 1)"
   name="${rel%% *}"; name="${name##*\\}"
   case "$rc" in
      -1073741515|-1073741701|14001|-1073741502)
         fail "$name does not start on this VM (exit $rc: a DLL or runtime it needs is missing)" \
            "Install the Microsoft Visual C++ 2005, 2008 and 2010 redistributables, x86 versions" \
            "(docs/BUILD-MACOS.md, step 6), then run this again." ;;
      9009) fail "cmd cannot run $name" "$out" ;;
      "") fail "could not run $name" "$out" ;;
      *) ok "$name starts (exit $rc)" ;;
   esac
done

echo "==> compile and link a test program"
remote "if not exist C:\\ps3-dev-check mkdir C:\\ps3-dev-check" >/dev/null
probe="$(mktemp "${TMPDIR:-/tmp}/sdkprobe.XXXXXX")"
printf '#include <stdio.h>\nint main(void) { printf("hello from the Cell SDK\\n"); return 0; }\n' > "$probe"
vm_scp "$probe" "$VM_HOST:C:/ps3-dev-check/probe.c" >/dev/null
rm -f "$probe"
GCC="$VM_SDK\\host-win32\\ppu\\bin\\ppu-lv2-gcc.exe"
# The same include and library paths build-ps3.bat passes.
out="$(remote "cd /d C:\\ps3-dev-check && del /q probe.o probe.elf 2>nul & $GCC -O2 -I$VM_SDK\\target\\ppu\\include -I$VM_SDK\\target\\common\\include -c probe.c -o probe.o && if exist probe.o echo COMPILED" || true)"
case "$out" in
   *COMPILED*) ok "ppu-lv2-gcc compiles - compiler and assembler run on this VM" ;;
   *) fail "ppu-lv2-gcc could not compile a hello-world:" "$out" ;;
esac
# Linking also proves the C runtime and the system stubs are found. It is only a warning when it fails: the
# app's link names its own libraries, and a toolchain that needs one more for a bare hello-world than it
# gets here can still build the app - ./build.sh has the final say.
out="$(remote "cd /d C:\\ps3-dev-check && $GCC probe.o -o probe.elf -L$VM_SDK\\target\\ppu\\lib -llv2_stub -lm && if exist probe.elf echo LINKED" || true)"
case "$out" in
   *LINKED*) ok "ppu-lv2-gcc links - the toolchain works on this VM" ;;
   *) echo "  warn  linking the test program failed; the build may still work - ./build.sh will tell:"
      printf '%s\n' "$out" | head -n 8 | sed 's/^/        /' ;;
esac

echo "==> source tree at C:\\ps3-dev"
have_tree=1
for dir in $TREE; do
   win="C:\\ps3-dev\\$(printf '%s' "$dir" | tr '/' '\\')"
   remote_exists "$win" || have_tree=0
done
if [ "$have_tree" = 1 ] && [ "$push_tree" = 0 ]; then
   ok "mohasi's libraries and the app are there (--push-tree replaces them)"
   echo
   echo "Build VM ready. Next: ./build.sh"
   exit 0
fi

# The tree goes up from the local clone, pinned and with this fork's files over it, so the VM builds
# exactly what this checkout describes.
"$ROOT/tools/fetch-upstream.sh"
"$ROOT/tools/sync-work.sh" --local
remote "if not exist C:\\ps3-dev\\libs mkdir C:\\ps3-dev\\libs" >/dev/null
remote "if not exist C:\\ps3-dev\\apps mkdir C:\\ps3-dev\\apps" >/dev/null
for dir in $TREE; do
   echo "  uploading ps3-dev/$dir"
   vm_scp -r "$ROOT/ps3-dev/$dir" "$VM_HOST:C:/ps3-dev/${dir%/*}/" >/dev/null
done
ok "source tree uploaded"
echo
echo "Build VM ready. Next: ./build.sh"

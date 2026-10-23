# Building on a Mac

There is no SDK manager to install, and no macOS version of anything Sony shipped for the PS3. The Cell
SDK is a directory of Windows programs (`C:\usr\local\cell\host-win32\...`): the compiler, the linker,
the shader compiler and the two packaging tools all run on Windows only.

So the build is split in two, as it always was:

- **The Mac** holds the source, drives every step and ends up with the `.pkg`. It needs nothing beyond
  what macOS already has, plus Apple's command line tools.
- **A Windows VM** holds the SDK and runs the compiler when the Mac asks it to, over ssh. On a Mac that
  VM runs in UTM (free), VMware Fusion or Parallels.

Once it is set up, a build is one command on the Mac:

```sh
./build.sh
```

Setting it up takes about an hour, most of it the Windows installation.

What you need beforehand: a copy of the Cell SDK, which is Sony's and is not part of this repository. Take
it from the machine the earlier builds were made on: the whole `C:\usr\local\cell` directory of that VM.

> **Not setting up a new VM?** If the old Windows VM is still running on the Linux PC, the Mac can use it
> as it is. Skip to [step 7](#7-buildenv-on-the-mac) and set `PS3_BUILD_JUMP` to the Linux PC, so ssh
> hops through it to the VM.

---

## 1. The Mac

```sh
xcode-select --install          # git and python3; ssh and scp are part of macOS
git clone https://github.com/TheErsysEnding/TEE-Vanced-PS3
cd TEE-Vanced-PS3
```

Nothing else is installed on the Mac: no Homebrew packages, no Python modules (only
`tools/make-branding.py` needs Pillow, and the artwork it renders is already in `branding/`).

## 2. A Windows VM

**Apple Silicon (M1 and later):** [UTM](https://mac.getutm.app), with Windows 11 for ARM.

1. Download the Windows 11 **Arm64** ISO from Microsoft
   ([microsoft.com/software-download/windows11arm64](https://www.microsoft.com/software-download/windows11arm64)).
   The x64 ISO does not start on an Apple Silicon Mac.
2. In UTM: **+** → **Virtualize** → **Windows** → choose the ISO. Leave *Install drivers and SPICE tools*
   ticked: without them the VM has no network. 4 GB of memory, 2 or more cores and a 64 GB disk are plenty.
3. Network: leave it on **Shared Network**. The VM then gets an address like `192.168.64.5` that the Mac
   can reach directly, with no port forwarding.
4. Install Windows. Once it is running, note the address (`ipconfig` in a command prompt, the *IPv4
   Address* line) and the account name (`whoami`, the part after the backslash).

The SDK's programs are 32-bit x86 programs. Windows 11 on ARM runs them through its built-in x86
emulation, without any setup. They run slower than on a PC, but a full build takes minutes, not hours.

**Intel Mac:** the same with the normal x64 Windows ISO, in UTM, VMware Fusion or VirtualBox.

## 3. An ssh server on the VM

In **PowerShell as administrator** on the VM:

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
Get-NetFirewallRule -Name *ssh*        # should list an enabled inbound rule for port 22
```

If the last line lists nothing, open the port yourself:

```powershell
New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
```

Leave the ssh server's default shell alone. It is `cmd.exe`, and every command the build sends is cmd
syntax.

## 4. A key, so the Mac logs in without a password

On the **Mac**:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/ps3vm -N ""
scp ~/.ssh/ps3vm.pub <account>@<vm-address>:C:/ProgramData/ssh/administrators_authorized_keys
```

The `scp` asks for the Windows password once. (If it answers *Permission denied*, write the file on the VM
instead, pasting the line `cat ~/.ssh/ps3vm.pub` prints on the Mac, in an administrator PowerShell:
`Set-Content -Encoding ascii C:\ProgramData\ssh\administrators_authorized_keys 'ssh-ed25519 AAAA...'`.
Use ASCII: a UTF-8 file with a byte-order mark is not read.) Then, in **PowerShell as administrator** on
the VM:

```powershell
icacls.exe C:\ProgramData\ssh\administrators_authorized_keys /inheritance:r /grant "*S-1-5-32-544:F" /grant "*S-1-5-18:F"
```

Why the odd file: for an account in the Administrators group, which a Windows account set up during
installation is, Windows' ssh server ignores `~\.ssh\authorized_keys` and reads only this shared file. It
also ignores that file unless only Administrators and SYSTEM can access it, which is what the `icacls` line
sets. It uses the groups' numeric IDs, so it also works on a German Windows, where the group is called
*Administratoren*. The two together are the usual reason the key "does not work" and ssh keeps asking for
a password.

Check it from the Mac:

```sh
ssh -i ~/.ssh/ps3vm <account>@<vm-address> echo works
```

`works`, and no password prompt: done.

## 5. The SDK

The SDK goes to `C:\usr\local\cell` on the new VM, exactly as it was on the old one. It is a large
directory with tens of thousands of files, so copy it as one archive. On the **old** build VM:

```bat
tar -a -c -f C:\cell.zip -C C:\usr\local cell
```

(`tar` comes with Windows 10 and later. On an older Windows, zip `C:\usr\local\cell` with 7-Zip.)

Bring `cell.zip` to the Mac, from the old VM through the Linux PC if that is where it runs:

```sh
scp -o ProxyJump=tee@linux-pc <old-account>@<old-vm>:C:/cell.zip ~/Downloads/
scp -i ~/.ssh/ps3vm ~/Downloads/cell.zip <account>@<vm-address>:C:/cell.zip
```

…or with a USB stick, or the shared folder UTM can set up. Then, in a **command prompt as administrator**
on the new VM:

```bat
mkdir C:\usr\local
tar -x -f C:\cell.zip -C C:\usr\local
setx /M SCE_PS3_ROOT C:\usr\local\cell
```

followed by `Restart-Service sshd` in an administrator PowerShell, so that ssh sessions see the new
variable. `SCE_PS3_ROOT` is what Sony's installer would have set; a copied SDK does not get it otherwise.

## 6. Runtime libraries, if a tool does not start

A fresh Windows can lack the Microsoft C runtimes some of the SDK's programs were built against. Step 8
tells you if that is the case ("does not start on this VM ... a DLL or runtime it needs is missing"). The
fix is Microsoft's **Visual C++ 2005, 2008 and 2010 redistributables, x86 versions**, from Microsoft's
download pages. The x86 versions install and work on Windows on ARM as well.

## 7. build.env on the Mac

```sh
cp build.env.example build.env
```

and set at least these two:

```sh
PS3_BUILD_VM="<account>@<vm-address>"      # e.g. tee@192.168.64.5
PS3_BUILD_KEY="$HOME/.ssh/ps3vm"
```

`build.env` is in `.gitignore`, so it never ends up in a commit. Two optional settings:
`PS3_BUILD_PORT` if the VM is reached through a port forward, and `PS3_BUILD_JUMP="tee@linux-pc"` if
the VM is the old one on the Linux PC and the Mac can only reach it through that PC.

## 8. Check the VM

```sh
./tools/setup-build-vm.sh
```

It goes through everything above and stops at the first thing that is wrong, saying what to do about it:
ssh, the shell, every SDK file the build uses, whether each SDK program starts, and finally an actual
compile and link of a test program on the VM. A fresh VM also gets its source tree here: mohasi's
libraries and the app, from `ps3-dev/` on the Mac, which the script clones first.

It ends with:

```
Build VM ready. Next: ./build.sh
```

## 9. Build

```sh
./build.sh
```

1. `tools/fetch-upstream.sh`: clones mohasi's tree into `ps3-dev/` on first use, at the commit this fork
   was built on, and refuses to build on a different one.
2. `tools/sync-work.sh --local`: copies this repository's `apps/` and `libs/` over it.
3. `genbuild.py`: writes `build-ps3.bat`, the compiler calls for the VM.
4. `tools/sync-work.sh`: uploads the sources and the batch file to the VM.
5. Compiles on the VM. The output goes to `pkg/out/build.log`, and the run stops unless it ends with
   `ALL-OK`.
6. Fetches `yo-player.ppu.elf` to `pkg/out/` and checks that it contains this version string and this
   run's build stamp. A build that silently links old code fails here, not on the console.
7. `build-pkg-fromsource.sh`: `PARAM.SFO` from `apps/yo-player/PARAM.SFO.xml`, then `make_fself_npdrm`
   and `make_package_npdrm` on the VM, then the `.pkg` back to the Mac.

The result is `pkg/out/TEE-Vanced-PS3-v0.12-beta.pkg`, with the version taken from `APP_VERSION` in
`apps/yo-player/include/app-paths.h`, plus its SHA256. `./build.sh --no-pkg` stops after step 6.

## 10. Onto the console

With webMAN's FTP server running, from the Mac's terminal (Finder can only read from FTP servers):

```sh
curl -T pkg/out/TEE-Vanced-PS3-v0.12-beta.pkg ftp://<ps3-address>/dev_hdd0/packages/
```

Then on the PS3: **Game → Package Manager → Install Package Files → Standard**. Or copy the `.pkg` to the
root of a FAT32 USB stick and install it from the stick.

---

## When something goes wrong

| What you see | What it means |
|---|---|
| ssh asks for a password | The key is not in `C:\ProgramData\ssh\administrators_authorized_keys`, or that file's permissions are too loose. Step 4. |
| `Connection refused` / `timed out` | The VM is not running, `sshd` is not started, or the address changed. Step 3; `ipconfig` on the VM. |
| `the VM's ssh shell is not cmd.exe` | Someone set PowerShell as the ssh default shell. Remove `DefaultShell` under `HKLM\SOFTWARE\OpenSSH`. |
| `SCE_PS3_ROOT is not set` | Step 5, last part, including the restart of `sshd`. |
| `does not start on this VM` | Step 6. |
| `ps3-dev/ is on <commit>, expected eb4be76` | The upstream clone was moved. `tools/fetch-upstream.sh --reset` puts it back. |
| `BUILD FAILED` | A real compile error. The full compiler output is in `pkg/out/build.log`. |
| `the .elf does not contain ...` | The VM linked something other than what was just compiled. Do not install that package. |

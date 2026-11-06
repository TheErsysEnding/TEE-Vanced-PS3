# TEE Vanced PS3

A YouTube client for the PlayStation 3, built from source and run on real
hardware. A fork of **[mohasi's Yo! Player](https://github.com/mohasi/ps3-dev)**
(Apache-2.0) — see [NOTICE](NOTICE); the application, its decoder and its
libraries are his work.

**Status: v1.0.1.** Every version here has been installed and used on a real
console before being called done. See [CHANGELOG.md](CHANGELOG.md).

---

## What this fork adds

**A left icon rail.** Search, For you, Trending, Live, Subscriptions, Continue
watching, Playlists, Watch Later, History, Login and Settings, all on screen as
icons instead of behind shoulder buttons you have to remember. LEFT or O opens it, X picks,
O or RIGHT close it again. The icons come from the icon font already compiled into the upstream
library — nothing extra is shipped.

**Your YouTube account, optionally.** Login, at the foot of the rail, shows a
code; type it at google.com/device on your phone and the app brings your
subscriptions, Watch Later, history, playlists and recommendations - on the
Account page and in the rail's own entries - and Subscribe buttons and Square
change your account. It signs in the way YouTube's own TV app
does - the password stays on Google's page - and keeps the sign-in as plain text
in `account.txt`, so keep the console's FTP to yourself. Playback never needs
the account.

**Live, and a region.** A page of streams that are on air now - Popular,
Gaming, Sports, Podcasts, News and Music - and a choice of 33 countries for
Trending, Live and the other lists.

**A settings screen.** Upstream has none: the options exist but the only way to
reach them is editing `settings.txt` over FTP. Autoplay and all seven
SponsorBlock categories are now switchable on the console, each showing its
state in colour so the screen can be read from a sofa.

**Three-way segment skipping.** Every SponsorBlock category is *Skip* (jump
automatically), *Skip by choice* (a button appears during the segment, the way
YouTube's own does) or *Keep*. That makes "skip the sponsor but let me decide
about the intro" something you can actually say.

**The PS button, on pads that do not have one.** A DualShock 4 or DualSense
pairs with a PS3 but its PS button never reaches the console, so there is no way
out of a running app. SELECT on the home screen and L3 during playback press it,
using libpad's public LDD virtual-controller API — no CFW plugin, no syscall.
This is believed to be the first PS3 *game*-context use of that API; every other
known user is a VSH plugin.

**Watch progress and Continue watching.** The resume position was always stored
and never shown. It is now a line along each thumbnail, and a category of its own.

**History, and unsubscribing from the list.** A browsable list of what you have
watched, and Triangle removes a channel where you are standing instead of making
you open it first.

**Pages, a search field with history, autoplay, chapter skipping.** Results are
paged rather than endlessly scrolled; the app keeps its own search history
because the PS3 keyboard keeps none.

**The original audio, and Square for the others.** YouTube serves more and more
videos with AI auto-dubbed audio tracks in other languages, and the player used
to take whichever track came last, so a video could start in a machine voice in
a language nobody asked for. It now plays the original. When a video has
several tracks, the player says so as it starts, and Square steps through them
from where you are.

**Its own look, "Ember".** A warm dark ground in TEE orange, matching the icon,
the Manrope typeface, and a focus that can be seen from the sofa.

**Your own playlists.** Local lists that nobody else can change - and, signed
in, your YouTube playlists above them. L3 files the selected video into a local
list; the sidebar opens them.

Not shipped yet: search filters (date, duration, HD).

---

## Building

You need mohasi's tree and the Cell SDK. This repository holds only the files
this fork changes — vendoring 300 MB of someone else's project would be worse
for everyone.

The Cell SDK is Windows software, so the compiler runs on a Windows machine with
the SDK at `C:\usr\local\cell` and an ssh server; everything else runs on the
machine you work on, macOS or Linux. **On a Mac, follow
[docs/BUILD-MACOS.md](docs/BUILD-MACOS.md)**: it sets up that Windows VM step by
step. There is no SDK manager or macOS port of the SDK to install.

```sh
cp build.env.example build.env      # which Windows VM builds, and with which ssh key
./tools/setup-build-vm.sh           # check the VM end to end; gives a fresh one its source tree
./build.sh                          # build on the VM, check the result, package it
```

The package lands in `pkg/out/`, named after `APP_VERSION` in
`apps/yo-player/include/app-paths.h`.

`build.sh` runs these steps, and each one also works on its own:

```sh
./tools/fetch-upstream.sh           # clone mohasi's tree into ps3-dev/, at the commit this fork is built on
./tools/sync-work.sh --local        # drop this fork's apps/ and libs/ over it
python3 genbuild.py                 # generate build-ps3.bat from the .vcxproj files
./tools/sync-work.sh                # mirror sources onto the build machine
ssh <vm> "cd /d C:\ps3-dev && build-ps3.bat"
./build-pkg-fromsource.sh TEE-Vanced-PS3-v1.0.1 "TEE Vanced PS3 V1.0.1"
```

Upstream is pinned (`UPSTREAM_REV` in `tools/build-env.sh`). mohasi's tree has
moved on since this fork was written, and laying these files over a newer
checkout would mix two versions of his libraries, so a plain
`git clone https://github.com/mohasi/ps3-dev` is no longer the starting point.

`genbuild.py` exists because upstream builds with MSBuild and the SN Systems
toolset, which needs Visual Studio; it reads the project files and emits direct
`ppu-lv2-gcc` calls instead, so the Cell SDK alone is enough.

`build-pkg-fromsource.sh` builds `PARAM.SFO` from `apps/yo-player/PARAM.SFO.xml`
(`pkg/make-param-sfo.py`), signs the ELF with `make_fself_npdrm` and packages it
with `make_package_npdrm`, both from the SDK. The artwork in `branding/` is
regenerated with `tools/make-branding.py` (Pillow).

**Verify what you built.** The build script stops on the first compile error and
wipes its object directory first — an earlier version did neither, linked a
stale object file and reported success, and shipped a package that silently did
not contain the change. `build.sh` now checks this itself: it fetches the ELF to
`pkg/out/` and refuses to package it unless it contains both the version string
and that run's build stamp. By hand, on the build machine:

```sh
ppu-lv2-strings yo-player.ppu.elf | grep "V1.0.1"
```

---

## Installing

Install the `.pkg` from the console's package manager. It writes its data to a
directory it picks at runtime — `/dev_hdd0/yo-player` cannot be created on every
console, so it falls back through a list and logs which one it chose to
`/dev_hdd0/tmp/TEE_Vanced/dbg.txt`. Your subscriptions, history and settings
live there and survive uninstalling the app.

A VPN on the console makes YouTube answer `LOGIN_REQUIRED`. Turn it off.

### Nothing loads: no channel names, no thumbnails

If the subscriptions list shows raw `UC...` ids and grey boxes instead of names
and pictures, the app reached none of its servers — the tiles come from your own
`subscriptions.txt`, only the names and images need the network.

**Power the console fully off and on again.** Not an XMB restart — a real power
cycle. This has fixed it for someone whose console had got into that state
overnight and stayed there across two different builds. Worth trying before
anything else.

If it survives a power cycle, check the console's date and time as well: the
certificate check uses the console's own clock, so a wrong date makes every
secure connection fail at once. `/dev_hdd0/tmp/TEE_Vanced/dbg.txt` names the
reason.

---

## Licence

Apache-2.0, the same as upstream — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
The TEE name, logo and the artwork in `branding/` are not part of that.

Contact: github@teebug.de

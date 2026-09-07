# TEE Vanced PS3

A YouTube client for the PlayStation 3, built from source and run on real
hardware. A fork of **[mohasi's Yo! Player](https://github.com/mohasi/ps3-dev)**
(Apache-2.0) — see [NOTICE](NOTICE); the application, its decoder and its
libraries are his work.

**Status: v0.12 beta.** Every version here has been installed and used on a real
console before being called done. See [CHANGELOG.md](CHANGELOG.md).

---

## What this fork adds

**A left icon rail.** Search, Trending, Subscriptions, Continue watching,
Playlists, Watch Later, History and Settings, all on screen as icons instead of
behind shoulder buttons you have to remember. LEFT or O opens it, X or RIGHT
picks. The icons come from the icon font already compiled into the upstream
library — nothing extra is shipped.

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

**Its own look.** Orange on warm black, matching the icon.

**Your own playlists.** Not YouTube's — there are none to read without a signed-in
account — but local lists that nobody else can change. L3 files the selected
video; the sidebar opens them.

Not shipped yet: search filters (date, duration, HD).

---

## Building

You need mohasi's tree and the Cell SDK. This repository holds only the files
this fork changes — vendoring 300 MB of someone else's project would be worse
for everyone.

```sh
git clone https://github.com/mohasi/ps3-dev
cp -r apps libs ps3-dev/            # drop this fork's files over it
python3 genbuild.py                 # generate build-ps3.bat from the .vcxproj files
```

`genbuild.py` exists because upstream builds with MSBuild and the SN Systems
toolset, which needs Visual Studio; it reads the project files and emits direct
`ppu-lv2-gcc` calls instead, so the Cell SDK alone is enough.

Then build and package:

```sh
./tools/sync-work.sh                # mirror sources onto the build machine
./build-pkg-fromsource.sh TEE-Vanced-PS3-v0.12-beta "TEE Vanced PS3 V0.12 Beta"
```

`build-pkg-fromsource.sh` signs the ELF with `make_fself_npdrm` and packages it
with `make_package_npdrm`, both from the SDK. The artwork in `branding/` is
regenerated with `tools/make-branding.py` (Pillow).

**Verify what you built.** The build script stops on the first compile error and
wipes its object directory first — an earlier version did neither, linked a
stale object file and reported success, and shipped a package that silently did
not contain the change. After building, check the change is really in the binary:

```sh
ppu-lv2-strings yo-player.ppu.elf | grep "V0.12 Beta"
```

---

## Installing

Install the `.pkg` from the console's package manager. It writes its data to a
directory it picks at runtime — `/dev_hdd0/yo-player` cannot be created on every
console, so it falls back through a list and logs which one it chose to
`/dev_hdd0/dbg.txt`. Your subscriptions, history and settings live there and
survive uninstalling the app.

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
secure connection fail at once. `/dev_hdd0/dbg.txt` names the reason.

---

## Licence

Apache-2.0, the same as upstream — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
The TEE name, logo and the artwork in `branding/` are not part of that.

Contact: github@teebug.de

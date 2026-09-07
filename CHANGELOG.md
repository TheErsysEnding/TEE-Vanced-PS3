# Changelog

Every release listed here was installed and used on a real PS3 (EVILNAT 4.92 DEX)
before it was tagged. Anything that had not been is said so plainly.

## v0.12 beta — 2026-09-06

### Added

- **Playlists.** Your own lists, kept on the console — there are no YouTube
  playlists to read or write without a signed-in account, so these are local and
  nobody else can change them. Up to 12 lists of 60 videos, stored in
  `playlists.txt` in the same format the watch-later queue already uses, so the
  app has one entry format rather than two to keep in step.
  - **L3** files the selected video, from the home screen or search results.
    The button came free when the grid layout was removed in v0.11.
  - The **Playlists** entry in the sidebar opens them: X opens a list as a feed,
    START creates one, Square deletes one behind a confirmation.
  - An opened playlist is just another home feed, so it inherits paging,
    thumbnails and caching unchanged.

### Fixed

- **Preferences were silently reset on every launch.** `readPref()` coerced every
  value to 0 or 1, which was invisible while the only settings were on/off flags
  — and then destroyed the ones that were not. The SponsorBlock category masks
  are seven-bit values, so "skip sponsors, self-promo and interaction" (7) came
  back as 1 and only sponsors survived a restart. The same clamp made the
  once-per-install subscription reconcile run on every boot.
- **The skip-by-choice offer acted on a stale frame.** The offer was recomputed
  *after* input was read, so on the first frame after the playhead left an "ask"
  segment, a Triangle press meant for the subtitles would still run the skip and
  jump the video forward.

## v0.11 beta — 2026-09-06

First public release.

### Added

- **Left icon sidebar.** Search, Trending, Subscriptions, Continue watching,
  Playlists, Watch Later, History and Settings, each as an icon. LEFT or O opens
  it, X or RIGHT picks, O leaves. Icons come from the icon font already compiled
  into the upstream library — nothing extra is shipped, and the PS3's own font
  has no magnifier, clock or gear.
- **A settings screen.** Upstream has none: its options exist but can only be
  reached by editing `settings.txt` over FTP, so in practice nobody finds them.
- **Three-way SponsorBlock control per category** — skip automatically, offer a
  button during the segment the way YouTube's own does, or ignore it. Each value
  shows its state in colour so the screen can be read from a sofa.
- **The PS button on pads that do not have one.** A DualShock 4 or DualSense
  pairs with a PS3 but its PS button never reaches the console, so there is no
  way out of a running app. SELECT on the home screen and L3 during playback
  press it, through libpad's public LDD virtual-controller API — no CFW plugin,
  no syscall. Every other known user of that API is a VSH plugin; this appears
  to be the first from a game.
- **Watch-progress bars** along each thumbnail, a **Continue watching** feed and
  a browsable **History** — all from a resume position that was already stored
  and had simply never been shown.
- **Unsubscribe from the subscriptions list itself** (Triangle), instead of
  having to open the channel first.
- **Real pages with a counter** instead of endless scrolling, and the app's own
  **search history**, because the PS3 keyboard remembers nothing typed into it.
- **Autoplay** and **chapter skipping** (L1/R1), plus L2/R2 for minute jumps.
- **Its own look** — orange on warm black — and an **XMB background**, which
  upstream does not ship at all.

### Changed

- The thumbnail grid was removed; the list is now the only layout. This freed
  L3, and START became the settings.

### Fixed

- **Nothing the user saved survived being written twice.** An existing file
  could not be overwritten where creating a new one succeeded, which made the
  subscribe button appear dead. Every save now removes the file and rewrites it
  if the first attempt fails.
- **User data no longer dies with the app.** It used to live under
  `/dev_hdd0/tmp`, which the console treats as scratch space, so deleting the
  package to install a newer build took the subscriptions and history with it.
  The data directory is now chosen at runtime from a candidate list and logged
  to `/dev_hdd0/dbg.txt`.
- **Paging stopped after one or two pages.** A single empty or failed
  continuation ended the feed for good; it now tolerates several.
- Selected rows in the list view drew white text on a white fill, making the
  highlighted title invisible.
- An autoplay hand-over reloaded the home feed and blocked the render loop on
  in-flight thumbnail requests at every video boundary.
- Autoplayed videos never entered the history, so the commonest way a session
  ends — walking away from a video that started itself — was invisible to both
  History and Continue watching.

## Before v0.11

This fork grew out of a fix rather than a plan. In August 2026 Google put a
PO-token wall in front of the InnerTube client Yo! Player used, and every video
longer than about a minute stopped playing. Switching the client to VISIONOS
brought them back; mohasi took that fix into his own source and credited it.
Everything above happened afterwards, in his tree, and is his work extended
rather than replaced.

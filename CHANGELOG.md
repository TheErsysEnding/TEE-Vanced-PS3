# Changelog

Every release listed here was installed and used on a real PS3 (EVILNAT 4.92 DEX)
before it was tagged. Anything that had not been is said so plainly.

## v2.0.0 — 2026-11-07

Installed and used on TEE's console (EVILNAT 4.92 DEX) through twelve test
builds on 2026-11-07 (V1.1 Test1 to Test10, V2.0.0 Test11 and Test12). The
XMB tile and background say what this version brings, and where to find TEE:
FULL HD 60 FPS, H.264 SUPPORT · TEE DECODER, linktr.ee/theersysending.

### Added

**TEE's own H.264 decoder.** The TEE Decoder (TEE's, on the SPEs, version
1.3.0) decodes YouTube's 1080p60 at 60 fps, where Sony's `cellVdec` manages
about 35 fps at 1080p. TEE Vanced now uses it for every video it can decode,
and opens 1080p60 (itag 299) for it; before, every 60 fps video played at
720p60. On the console 1080p60, 720p60, 360p and 144p all played on it, with
sound, seeking and switching videos - and live streams in 1080p60.

- It decodes High and Main profile H.264 with CABAC, 8 bit 4:2:0, progressive,
  with a coded width that is a multiple of 128 up to 1920 x 1088 - YouTube's
  1920-, 1280-, 640- and 256-wide streams. The app reads each stream's own SPS
  and PPS before it opens the decoder; everything else, the 480p/240p sizes
  (854 and 426 wide) among it, stays on `cellVdec`. A 1080p60 video that does
  not get the TEE Decoder plays at 720p60 on `cellVdec`, as before.
- It uses 4 SPEs (1 CABAC + 3 pixel), `cellAdec` 1 more for the sound: 5 of 6,
  as with `cellVdec`. `tee-decoder-spes=5` in `settings.txt` gives it a fourth
  pixel SPE (all 6 busy with the sound).
- `video-decoder=sony` in `settings.txt` plays everything on `cellVdec` again.
- If it fails while playing, the app flushes it and goes on from the next
  keyframe; a second failure within 30 s hands the video to `cellVdec` until
  the app is restarted.
- The module is vendored unchanged in `libs/simple-lib-av/src/tee_decoder/`;
  the build stops if it differs from `libs/simple-lib-av/tee_decoder.sha256`.

**Smooth 60 fps on a 59.94 Hz screen (display sync).** A video whose frame
rate is the screen's - 60 fps on the PS3's 59.94 Hz, or 30 fps at two vblanks
a picture - now shows exactly one picture per vblank, and the sound plays
0.1% slower to match (1.7 cents, not audible), steered so picture and sound
stay together. Before, the clock had to drop a picture every 16.7 s, and the
audio clock's 5 ms steps turned that into a few seconds of judder each time.

**Choose the quality.** START shows the controls; with them up it opens
Options: Quality, Chapters and Autoplay. Quality lists Auto and every quality
of the video the player can play - for example 1080p60, 720p60, 360p, 144p.
A pick switches at the same position and stays as the highest quality for the
next videos too (`quality=` in `settings.txt`, e.g. for a slow connection),
until Auto is picked again.

**Live streams: rewind, quality, 1080p60.** A live stream used to play only
from where it was, in 720p60 at most.

- Where the streamer left YouTube's rewind (DVR) on, left/right go 5 s back
  or ahead and L2/R2 a minute, up to 12 hours back, on a timeline of the
  stream's window; R1 goes back to live. The player is rebuilt at the target -
  a second or two of "Loading..." - and starts at the beginning of the segment
  it lies in, up to one segment length (5, 2 or 1 s, by the stream) before it.
- Where the streamer turned it off, there is no rewind, as on YouTube, and a
  press says so. That was 4 of 6 streams in one look on 2026-11-07, mostly news.
- The LIVE badge is red at live; behind it - a rewind, a pause - it turns dark,
  after how far behind ("-1:23").
- START > Quality lists a live stream's qualities too and switches at the
  segment playing, or at live when it was live.
- 1080p60 live (itag 299) plays on the TEE Decoder.
- A live stream that has just ended - which YouTube serves for a while only
  as its live segments, until it has made a video of it - plays as its
  recording: from the start, or from where it was left, with a video's
  timeline and time, left/right and L2/R2 to move through it, and Replay at
  its end. It used to say "couldn't open stream" (in Yo! Player too).
- Played on the console: 1080p60 live with 1 s and 2 s segments for minutes
  at a time, rewinds up to 70 minutes back and back to live, quality switches
  between 144p, 480p, 720p60 and 1080p60, and the 69-minute recording of a
  1080p60 stream that had ended an hour before, moved through both ways.

### Changed

- O puts the controls away first, paused or not; only with nothing on the
  picture does it leave the video. It used to leave at once.
- START opens Options instead of the chapter list (Chapters is in it), and
  Autoplay moved there from Square, which is now only the audio-track button.
- The R3 panel names the decoder and says "Press R3 to hide"; on a live
  stream it shows how far behind live it plays, the segment and whether the
  rewind is on.
- R1 on a live stream goes back to live (on a video it is still the next
  chapter).

### Fixed

- **The app hung after seeking back from near the end of a video** (also in
  Yo! Player). Once the last stretch of the file had been fetched, a seek back
  was never fetched again: the picture stopped, and O froze the app (FTP and
  webMAN still answered). The fetch now follows the seek, closing the player
  cancels a read that waits, and a read that gets nothing for 30 s gives up.
- **A live stream could stop with the sound playing on, and O then froze the
  app** (also in Yo! Player, rarely). The live source cut a segment short when
  its 8 MB buffer was full, and 1080p60 live has segments of up to 4 MB a 2 s
  segment and up to 10 MB a 5 s one: the player then read the next segment's
  bytes as picture data and waited for good. Every segment now goes in whole,
  into a buffer sized for it.

### Known issues

- **Long live sessions fall behind live by 3.6 s an hour.** To show 60 fps
  without judder on the PS3's 59.94 Hz output, the player runs 0.1% slow (see
  display sync above), so after an hour on one live stream the picture is about
  3.6 s further behind live and the badge says "-0:04". R1 catches up. Keeping
  up by itself would mean dropping a picture every 16.7 s - the judder display
  sync took away.
- **A rare hang after a decoder error.** If one of the TEE Decoder's pixel SPEs
  ever reports an error, stopping the decoder (a seek, leaving the video) can
  wait for good. The app no longer freezes on it: leaving a video waits 5 s at
  most, then leaves the decoder behind and plays everything on `cellVdec` until
  the app is restarted (the next video may not open before that). It has not
  happened in any test; if it does, please say so in the TEE Vanced thread on
  PSX-Place, with `/dev_hdd0/tmp/TEE_Vanced/dbg.txt` if you can.
  `video-decoder=sony` in `settings.txt` plays everything on `cellVdec`.

### Licence

- The TEE Decoder (`libs/simple-lib-av/src/tee_decoder/`) is under the GNU
  LGPL 2.1 or later, as parts of it follow FFmpeg's libavcodec; the rest of
  TEE Vanced stays Apache-2.0. NOTICE lists the runtime parts its SPU programs
  carry (GCC runtime, PSL1GHT, newlib).
- Its SPU programs no longer carry the build machine's path in their names.

## v1.0.1 — 2026-11-06

### Changed

**TEE Vanced installs beside Yo! Player instead of over it.** Up to v1.0 the
package used Yo! Player's title id (`YOPLAYER1`) and its folder names, so
installing TEE Vanced replaced mohasi's Yo! Player, and the two would have
shared one set of files. It now has its own title id, `TEEV00001`, and its own
icon on the XMB.

Its data moves to folders of its own as well (`tee-vanced` instead of
`yo-player`, for example `/dev_hdd0/home/00000001/tee-vanced` or
`/dev_hdd0/game/TEEV00001/USRDIR/data`). On the first start the app copies
subscriptions, playlists, Watch Later, history, settings, themes and the
YouTube sign-in over from the old folders; the old files are left as they were.

**Start the new version once before you delete the old icon.** On some
consoles the data of v1.0 lives inside the old app's own folder, and the
console deletes that folder together with the app. After the first start the
old "TEE Vanced PS3" icon (the one that replaced Yo! Player) can be deleted.

## v1.0 — 2026-11-06

The first full release. The new look, "Ember": a warm dark ground with TEE
orange, the Manrope typeface, thumbnails that carry the pages, a focus that can
be seen from the sofa, and the console's own button symbols. Every button still
does what it did, apart from LEFT, UP and RIGHT (below). New: signing in to
YouTube, a region for the lists, and a page of live streams. It also brings the
fixes of v0.13.1, which never came out on its own.

### Added

- **Sign in to YouTube**: **For you**, under Search in the menu, and **Login**,
  at its foot above Settings, show a code and a QR code; you open
  google.com/device on your phone, sign in to Google there and type the code -
  the PS3's own YouTube app signed in this way, and the password never touches
  the console. Signed in, For you holds your recommendations, and Login reads
  **Account** and opens your account's page: For you, Subscriptions, All
  channels A-Z, Watch Later, History, Liked videos and Playlists, and **Sign
  out** at its foot. The menu's own entries show your account too:
  - **Subscriptions** shows the channels your account follows, one card each
    with its name and its newest video, the ones with something new first; X
    opens a channel, Triangle unsubscribes. **All channels** on the account
    page lists every one of them A to Z, with its subscriber count.
  - **Watch Later** and **History** are your account's, and **Playlists**
    lists your YouTube playlists (Liked videos and your own) above the ones
    kept on the console; X opens them.
  - A channel page has a real **Subscribe** button beside its sorts - orange
    "Subscribe", or "Subscribed" with a check. UP from the first video reaches
    it, X toggles; Triangle still does from anywhere on the page. Signed out it
    works on the console's own subscriptions, as Triangle always did.
  - **Square** puts a video in or out of your account's Watch Later. YouTube
    is told at once; should it refuse a change, it is undone and a box says
    so.
  - Continue watching, the console's playlists and playback stay as they were:
    videos play without the account, so a sign-in YouTube stops accepting can
    never stop a video. Editing your YouTube playlists comes later.
  - The sign-in is kept in `account.txt` in the data folder, as plain text:
    whoever can read the console's disk - webMAN's FTP has no password unless
    you set one - can use your YouTube account with it (not your Google
    password). **Sign out** on the account page withdraws it at Google and
    deletes the file. The app signs in as YouTube's TV app does, which is not
    an official way for other apps; Google may stop accepting it one day.

- **Region**: the country YouTube's lists come from, in the settings under
  Autoplay. X opens a list of 33 countries - Germany, Austria and Switzerland
  first, then A-Z, L2/R2 a page at a time - and every list follows it:
  Trending and Live fully, Search and Trending Sports only partly (part of them
  follows where the console is). The app stays in English, because it reads
  YouTube's English text. United States stays the default, as before.
- **Live**: a page of streams that are on air now, in the menu after
  Trending, with Popular, Gaming, Sports, Podcasts, News and Music. L1/R1 move
  between those six only; R3 is gone there, as a live stream cannot be
  downloaded. Where News has too few streams, a live search for the country's
  own word for news fills it up. A Live page older than ten minutes reloads
  when you come back to it, since streams end.

### Changed

- **The home screen is a grid** of four cards a row and two rows a page, under
  the page title, the Trending categories as chips, the page count and a clock
  (12 or 24 hours, as the XMB is set). A card shows its duration or LIVE, how
  far you got and the Watch Later tag. A finished video is dimmed and marked
  WATCHED; one you stopped part way shows only its progress bar - before,
  every started video was dimmed. Subscriptions are channel cards in the same
  style.
- **LEFT moves along the row** now that there are four columns. Only from the
  first column does it open the menu; O still opens it from anywhere. RIGHT
  stops at the last column instead of running on into the next row, so LEFT
  always undoes it, and DOWN onto a shorter last row lands on its last card.
- **The menu** is a narrow strip of icons under the TEE tile. Opened, it lies
  over the page with the names and dims the rest. Search, the search field,
  the settings and the playlists (opened from the menu) show it too, and LEFT
  opens it there as on the home screen; picking another page there goes back
  to the home screen and opens that page. RIGHT now closes the menu, as O
  does, and leaves you where you were; only X opens the highlighted page
  (RIGHT used to open it too).
- **Search**: the field and the search history as rows; the results three to
  a page, with the sort shown as a switch (SELECT changes it as before) and
  the page count in the field. UP on the first result moves to the field, and
  X there opens the keyboard on your query - no need for START. On the field
  screen the hints say what X and Square do on the row you are on.
- **The settings are one list** - Playback, then SponsorBlock with Skip, Ask
  and Keep for each category - beside the About card with the QR code.
- **The player**: title, channel and views at the top left; quality, audio
  track and subtitles as chips at the top right; the chapter and the time over
  a new timeline with gaps between chapters, the SponsorBlock segments in
  their colours and a legend for the ones the video has; the button hints
  along the bottom. The volume is a bar with the percent on the left, and the
  offer to skip a segment a panel that counts down the time left.
- **Playlists** and the download badge in the same style.
- **A new palette** from the Ember design: the same TEE orange on a warm dark
  ground with cream text, and a red of its own for the LIVE badge. A
  `themes.txt` written by an older version still holds the old colours, and its
  `[TEE]` block would have hidden the new ones for good - so it is copied to
  `themes.txt.bak` once and replaced. A theme of your own in it survives in the
  `.bak`. The `[YouTube Classic]` template has values for every new key.
- **Manrope** replaces the console font for the app's own text, in four
  weights embedded in the app (SIL Open Font License, see `NOTICE`). A title,
  channel name, search or playlist name with a character Manrope does not have
  (Japanese, Korean, emoji ...) is drawn entirely in the console font, as
  before. Text from YouTube is no longer read as markup, so a `{` in a title
  cannot swallow the rest of it.
- **No text below 20 px** except badges and key captions, which are 18 px in
  extra bold.
- **New button hints**: the console's own button symbols, larger and inside
  the TV's safe area. On the home screen Play, Watch Later and Channel sit at
  the top right and the others at the bottom right; in the settings they are
  at the bottom left, and the player has a row of its own along the bottom.
- **SponsorBlock categories** go by their full names everywhere, in the
  settings and in the player's "Skip ..." and "Skipped ..." messages:
  Self-promotion, Interaction reminder, Outro and end cards, Filler and
  tangents. Their colours follow the new design, and Off-topic music turned
  pink - its orange was all but invisible on the orange played part of the
  seek bar.
- The download badge sits inside the safe area, clear of the hints and the
  cards.
- In the player the times are drawn digit by digit at a fixed width, on the
  new shade at the bottom of the picture: they keep their place as they count
  and stay readable on a bright picture.
- **The video description** is set in Manrope as well, up to about 70 lines.
  Manrope's lines are taller, so a longer description stays in the console
  font, which fits more of them, and nothing that showed before is cut.

### Fixed

- **Closing the description or the chapter list while Up or Down was still
  held changed the volume at once.** A direction held across the close now
  does nothing until it is released or its normal repeat delay has passed.
- **Leaving the player with a direction still held made the home or search
  list jump** straight away, and keep jumping. A direction held across the
  return now waits for the usual repeat delay; a fresh press still moves the
  list at once.
- **A description too long for the text renderer left the description
  empty.** The renderer refuses a text taller than its 16 MB limit; such a
  description is now shortened from the end until it fits, and ends in "...".
- R3's statistics gave a long video's time in minutes (192:50); they now use
  the hours the timeline shows (3:12:50).
- **Replay at the end of a video hung.** X sought the finished streams back to
  the start; they only starved, the time stood at 0:00, and O then froze the
  app. Replay - and any seek once a video has ended - now reopens the video at
  that point, as an audio-track switch does. Found in RPCS3.
- **R1 landed a few seconds before the next chapter**, on the last keyframe
  before its start, so the previous chapter's name showed. Chapter jumps (L1,
  R1, the chapter list) now land at or just after the chapter's start.
- **A channel's Popular and Oldest showed the Latest list.** YouTube moved the
  three sorts into a menu on one "Latest" chip; the app now reads them there.
- **Trending cards had no duration**: YouTube gives it there only on the
  thumbnail, where the app now reads it.
- **Search never showed LIVE badges**, and R3 queued a live hit for a download
  that could only fail: search marks a live stream only with a badge the app
  did not read. Live hits now show LIVE and "... watching", and R3 leaves them.
- **Names and titles cut by the app's field sizes** ended in the middle of a
  word, or of a Cyrillic or Japanese character, which showed as a stray letter.
  They are cut at a whole character now and end in "...".
- **A title with small superscript letters lost letters elsewhere**: "ᴴᴰ" sent
  "SÖZCÜ TV Canlı Yayını ᴴᴰ" to the console font, which has no Turkish dotless
  i, so it read "Canl  Yay  n". Such letters now read as the plain ones ("HD")
  and the title stays in Manrope.

## v0.13.1 beta — never released on its own, part of v1.0

Confirmed on the console on 2026-11-05, then released only as part of v1.0.

### Fixed

- **v0.13 lost every change this fork had made to mohasi's libraries.** They
  were made on the build machine on 2026-09-01 and never committed, so v0.13 -
  the first release built from a clean checkout - shipped without them. Found
  by comparing the released v0.12 and v0.13 binaries function by function; the
  sources were recovered from the session they were written in, and they match
  the v0.12 binary. They now live in `libs/`, laid over mohasi's tree at every
  build:
  - **The log** (`dbg.h`), now in `/dev_hdd0/tmp/TEE_Vanced/dbg.txt`. v0.13
    wrote to `/dev_hdd0/tmp/dbg.txt`, which other homebrew uses as well and
    may have left writable for itself only, so no log arrived anywhere. v0.11
    and v0.12 used `/dev_hdd0/dbg.txt`, but not every console lets an app
    create a file directly in `/dev_hdd0`. The app now writes into a folder of
    its own in `/dev_hdd0/tmp` and creates it whenever it is missing.
  - **60 fps catch-up** (`video-player.c`, `decode-h264.c/.h`). On a stream
    above 30 fps the decoder may drop non-reference B pictures while the
    picture is more than 250 ms behind the clock, until it is back within
    60 ms. Confirmed on 720p60 with v0.12; v0.13 played such videos without it.
  - **Live streams** (`live-source.c`). The live fallback in `youtube.c` hands
    over a segment address ending in `=`, which needs the number appended
    directly; v0.13 built a wrong address from it.
  - **The L3 button icon** (`console-glyphs.c/.h`). The home and search hints
    use it, so the repository did not even compile without it; v0.13's build
    machine had a stand-in of its own.
- `build.sh` now refuses a binary that lacks the log path or the catch-up
  message, so a build without `libs/` cannot slip through again.
- **The YouTube session was never saved where `/dev_hdd0/yo-player` is
  refused.** `visitor.txt` and `settings.txt` were still built from that fixed
  path rather than from the data directory the app picks at runtime, so on
  such a console neither file was ever written. Every launch began a new
  anonymous YouTube session, although the cache exists because a new one each
  time looks like a bot, and `settings.txt` was never created, so there was
  nothing to edit over FTP. Both now live in the data directory with
  everything else, as `themes.txt` already did. Present since v0.11.
- **RIGHT could get stuck.** Every seek went to the last keyframe before its
  target, steps forward included, so where two keyframes were more than
  RIGHT's 5 seconds apart, each press landed back on the first of them -
  sometimes a second behind where the video had been. A step forward now
  lands on the first keyframe at or after its target. That holds for the DASH
  streams YouTube normally serves; a plain mp4 still rounds down. Present
  since v0.11.

## v0.13 beta — 2026-11-04

### Fixed

- **Videos played an AI-dubbed audio track instead of the original.** YouTube
  serves many videos with extra audio tracks: the uploader's own other languages,
  and AI auto-dubs. They are separate streams with the same itag, and the player
  took the last itag 140 on offer, so whichever language YouTube listed last.
  With many dubs there was a second fault: at two or three AAC streams per
  track, the 24-entry format list could fill up before the original was
  reached, and then a dub played at 48 kbit/s (itag 139). Each track is now recognised
  from the reply (`audioTrack`, and the url's `xtags` where that is missing),
  only the best stream of each track is kept, and the original plays (else
  YouTube's default track, else any track that is not a dub). Reported on
  psx-place: "Yo! Player plays AI generated audio on random videos". Downloads
  pick the same track.
- Where YouTube serves a dynamic-range-compressed copy of the audio beside the
  normal one, the normal mix is now preferred; the old rule could land on
  either.

### Added

- **Square switches the audio track** on a video that has more than one. The
  player reopens on the next track at the same position and names it. As such
  a video starts, a notice says which track is playing and how many there are.
  On every other video Square still toggles autoplay, which is also in the
  settings.

### Build

Changes to how the app is built, so it can be built from a Mac. None of them
changes what the app does.

- **Building from macOS.** Every script on the building side now runs on the
  bash 3.2 and Python 3.9 macOS ships, and `docs/BUILD-MACOS.md` sets up the
  Windows VM that the Cell SDK needs, in UTM on the Mac.
- **`build.sh`** does the whole build in one run and refuses to package an ELF
  that does not contain this run's version string and build stamp.
- **`tools/setup-build-vm.sh`** checks a build VM from ssh to an actual compile
  and link, and gives a fresh one its source tree.
- **Upstream is pinned** to eb4be76, the head of mohasi's tree when this fork
  was built. The README used to say `git clone` his tree as it is today, which
  has since replaced `pad.c` and added to the libraries this fork builds on.
- **`sync-work.sh` works from a plain checkout.** It needed a `work/` directory
  that only ever existed on the original build machine, and it never uploaded
  this fork's `pad.c` to the VM.
- **`PARAM.SFO` is built from `PARAM.SFO.xml`** instead of being patched into
  one extracted from mohasi's released package. The result is the same file as
  in v0.12 except for one field: TITLE's used length, which the in-place patch
  left at 11 bytes, the length of "Yo! Player".
- The repository's `patch-param-title.py` ignored the title it was given and
  always wrote an old placeholder one. It now uses the title it is given.

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

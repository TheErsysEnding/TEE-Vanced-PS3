#pragma once

// theme.h - yo-player's palette as one runtime-swappable struct, so every colour the UI draws comes
// from one place. The shipped theme is TEE's own palette - since V3.0.0 ice blue (#38BDF8) as the only accent
// on a deep blue-black, Ember's tokens in another hue (v1.0 - v2.0 were warm near-black with TEE orange);
// rounded shapes are white helper textures tinted with these colours (ui-draw.h).
//
// Themes live in themes.txt in the app's data directory (getAppDataDir(), see app-paths.h), written on
// first launch with the built-in [TEE] block so every key is visible to edit over FTP. Edit that block to
// restyle the app, or add a [Name] block of your own and point settings.txt at it with "theme=<name>"
// (lower case, spaces -> hyphens). There is no in-app theme switcher: colours are read at startup and
// apply on next launch.

#include <stdint.h>

typedef struct {
   uint32_t appBg;              // whole-screen background (Ember "bg"); also the colour list fades end in
   uint32_t surface;            // raised surface (Ember "surface-2"): chips, fields, the rail, the thumbnail
                                // placeholder before the image lands
   uint32_t accent;             // the accent (ice blue): seek-bar fill, progress, the selected value, volume fill
   uint32_t focusBorder;        // the focus ring around the selected thumbnail, row or card
   uint32_t focusGlow;          // tints the soft glow texture drawn under the focus ring (Ember
                                // "accent-glow") - the console has no blur, so the glow is a texture

   uint32_t textPrimary;        // titles, values, durations, times (Ember "text-1")
   uint32_t textSecondary;      // channel / views / age lines, button hints, descriptions (Ember "text-2")

   uint32_t badgeFill;          // translucent black behind every small overlay box (badges, stats, subtitles)
   uint32_t scrim;              // dims the video behind a full-screen overlay (description, chapters)
   uint32_t rowHighlight;       // selected row in the chapter picker

   uint32_t seekTrack;          // unplayed part of the seek bar (Ember "zeitleiste")
   uint32_t seekNotch;          // chapter boundary marks on the seek bar (the gaps between chapters in the
                                // Ember timeline)
   uint32_t watchedThumbTint;   // already-watched tiles: opaque darkening applied to the thumbnail

   // ---- Ember tokens (UEBERGABE 3.1) that the struct did not have before v1.0 ----
   uint32_t surface1;           // "surface-1": cards, panels, the focused search row
   uint32_t surface3;           // "surface-3": the chosen value while it is not focused ("calmly selected")
   uint32_t textTertiary;       // "text-3": channel . date in cards - only at 20 px or larger (18 px grey
                                // text was unreadable on the TV once already, see theme.c)
   uint32_t accentSoft;         // "accent-soft": the surface of a selected item (rail entry, chip), 20 %
   uint32_t accentText;         // "accent-text": text on accentSoft
   uint32_t onAccent;           // "on-accent": text on the solid accent (logo tile, selected segment)
   uint32_t live;               // the LIVE badge
   uint32_t knobGlow;           // glow around the timeline knob in the player (accent at 50 %)
   uint32_t hintFill;           // "tasten-flaeche": plate behind key symbols on page backgrounds (.12)
   uint32_t hintFillOnVideo;    // the same over the video (.14)
   uint32_t chipOnVideo;        // "auf-video": chips over the video (.72)
   uint32_t panelOnVideo;       // "auf-video": panels over the video - skip offer, toasts, volume (.86)
   uint32_t progressTrack;      // unplayed part of a started card's progress bar inside its thumbnail
   uint32_t hairline;           // thin separators and 2-px inset borders on the dark surfaces
   uint32_t gradientBase;       // the dark the player's top and bottom gradients fade into (alpha per stop)
   uint32_t letterbox;          // behind the video picture: the bars around a frame that does not fill 16:9

   int focusThickness;          // width of the focus ring in px (Ember: 4; the ring sits 5 px off the card)
} Theme;

// the palette in use; never NULL (points at the built-in TEE theme until initTheme runs).
extern const Theme *activeTheme;

// reads themes.txt (creating it from the built-in when missing, or replacing it - after a backup to
// themes.txt.bak - when it predates the current palette) and selects the theme settings.txt names. call
// once at startup, after the VFS is up and before anything draws or rasterises a label.
void initTheme(void);

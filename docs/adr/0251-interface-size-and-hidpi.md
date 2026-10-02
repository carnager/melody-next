# ADR-0251: Trackknife scales with the screen, and an interface size on top

## Status

Accepted, 2026-10-02.

## Context

Trackknife runs on Linux (X11 and Wayland, often at 125 % or 150 %), on
Windows (per-monitor scaling) and on macOS, where a Retina or 5K display draws
at two device pixels to each logical one. Qt 6 scales windows by the
desktop's factor by itself, and the widgets window laid out correctly at 1.5x
and 2x. What did not scale were pixmaps made at a fixed pixel size: the tag
editor's 108 px cover, the artwork lists' 60 px thumbnails, the drag pill and
the playback speaker icon were drawn at one device pixel per logical one and
came out soft. And a desktop can report too little -- X11 without a DPI
setting -- or a user can want Trackknife larger or smaller than the rest.

## Decision

- **Pixmaps in device pixels.** What Trackknife draws or decodes for display
  carries the device pixel ratio: covers and thumbnails are decoded at three
  device pixels per logical one (sharp at 2x and 3x, drawn down at 1x), drawn
  pixmaps are made at the widget's `devicePixelRatioF()`, and drawn icons
  carry 1x and 2x sizes.
- **Interface size**, Settings › General › Appearance: *As the system
  (100 %)*, 75 %, 90 %, 110 %, 125 %, 150 %, 175 %, 200 % -- stored as
  `appearance/interface-scale` and applied, before the application is made,
  as `QT_SCALE_FACTOR`, multiplying whatever the desktop already applies. It
  takes effect at the next start. A `QT_SCALE_FACTOR` in the environment wins;
  screenshot runs ignore the setting. Both windows read it.

## Consequences

- A Retina or 5K Mac, a 4K Linux desktop at 150 % and a Windows laptop at
  125 % all get Trackknife at the desktop's size with sharp covers and icons,
  without configuration.
- A user on a desktop that misreports its density, or who wants another
  size, sets it once; it needs a restart, as Qt fixes the scale at start.

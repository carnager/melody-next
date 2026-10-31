# ADR-0345: The command palette offers every menu entry; the library engine from a menu

## Status

Accepted, 2026-10-31. Supersedes ADR-0205's curated inventory; the rest of
ADR-0205 (search, keys, availability, one window) stands.

## Context

ADR-0205 gave the palette a hand-kept list of commands and left out output
names, modes and column labels as "parameter choices". The list fell behind:
Repeat, Random, ReplayGain modes, the playback buffer, the speakers, Panels,
Queue next and more were in the menus or on keys but not in the palette, and
one entry named an action that no longer existed. Switching speaker, or
which engine holds the library, had no keyboard path at all: the library
moved only in Settings → Engines.

## Decision

- **The palette reads the window.** Every entry of the menu bar, in every
  submenu, then the window's own keyed actions (Queue next, Search the
  library, Panels...), each once. Nothing is listed by hand, so a new menu
  entry is a palette command. Context menus stay out: what they act on is
  set when they open.
- **A submenu's entry is named under it**, `Output › gemenon`,
  `Playback buffer › Resilient`; the menu bar's menu is searched but not
  shown. A submenu greyed out or hidden hides what it holds.
- **Workspace › Library engine** names this computer and each engine added in
  Settings → Engines, the one holding the library checked. Choosing one is
  choosing it there and saving: a laptop takes the home server's library at
  home and its own away (transcodes prepared for the road), from the
  palette. Each engine keeps its own folders, so nothing is lost moving
  back and forth. Hidden with no engine elsewhere. Named *Library engine*,
  the term of Settings → Engines: plain *Library › gemenon* read as one
  library of several.
- Which engine the controls follow is not a menu choice: a tab plays on its
  engine, and another client starting one is followed (ADR-0227). A first
  build of this ADR offered it as Playback › Engine; it read as the library
  and went.
- **Playback › Output** is the output button's menu: the engines' speakers
  and the chosen one's devices. It replaces the Playback menu's own
  Refresh audio devices.
- The shortcut editor's list of key-able commands without a default key is
  unchanged.

## Verification

`commandPaletteOffersEveryMenuEntry`, `commandPaletteHidesWhatAGreyedMenuHolds`
and `theLibraryMenuMovesTheLibrary` in the bench window tests.

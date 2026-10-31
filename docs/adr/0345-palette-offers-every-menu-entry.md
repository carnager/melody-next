# ADR-0345: The command palette offers every menu entry, and the engine followed is chosen

## Status

Accepted, 2026-10-31. Supersedes ADR-0205's curated inventory; the rest of
ADR-0205 (search, keys, availability, one window) stands.

## Context

ADR-0205 gave the palette a hand-kept list of commands and left out output
names, modes and column labels as "parameter choices". The list fell behind:
Repeat, Random, ReplayGain modes, the playback buffer, the speakers, Panels,
Queue next and more were in the menus or on keys but not in the palette, and
one entry named an action that no longer existed. Switching speaker, or
which engine the controls follow, had no keyboard path at all: the window
followed an engine only when one of its tabs played or another client
started it.

## Decision

- **The palette reads the window.** Every entry of the menu bar, in every
  submenu, then the window's own keyed actions (Queue next, Search the
  library, Panels...), each once. Nothing is listed by hand, so a new menu
  entry is a palette command. Context menus stay out: what they act on is
  set when they open.
- **A submenu's entry is named under it**, `Output › gemenon`,
  `Playback buffer › Resilient`; the menu bar's menu is searched but not
  shown. A submenu greyed out or hidden hides what it holds.
- **Playback › Engine** names each engine with a playback connection,
  `Follow this computer`, `Follow gemenon`, the followed one checked. Choosing
  one follows it without stopping the other, as when another client starts
  it (ADR-0227's one-engine-plays rule applies when something is played).
  Hidden with one engine.
- **Playback › Output** is the output button's menu: the engines' speakers
  and the chosen one's devices. It replaces the Playback menu's own
  Refresh audio devices.
- The shortcut editor's list of key-able commands without a default key is
  unchanged.

## Verification

`commandPaletteOffersEveryMenuEntry`, `commandPaletteHidesWhatAGreyedMenuHolds`
and `theEngineMenuFollowsTheEngineChosen` in the bench window tests.

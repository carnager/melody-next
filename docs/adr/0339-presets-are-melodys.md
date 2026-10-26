# ADR-0339: Presets are Melody's

## Status

Accepted on the branch `mpd-client`, 2026-10-26 -- the first slice of
"MPD and a powerful client": a server that keeps the library, lists and
playback, and Melody, which keeps the user's presets and does the file work.
Not on main.

## Context

Melody's presets -- naming layouts, move destinations, encoder presets,
tagging scripts -- lived in the database of this computer's engine, which
the window opened directly, and naming layouts were copied to every other
engine (`layouts.put`). The copies drifted: a layout left on a home server,
invisible in Settings, because an engine never takes away what it is not
told to. And melody-cli named files by the layouts of whichever engine it
asked, not by the user's.

## Decision

- **Melody keeps its presets in its own database**,
  `presets.sqlite` in Melody's data folder (`$XDG_DATA_HOME/melody/melody`,
  on macOS `~/Library/Application Support/melody/melody`) --
  `core::presets_database_path()`, the one path the window and melody-cli
  both use. The window no longer opens the engine's database.
- **Taken over once**: when Melody's database does not exist yet and this
  computer's engine's does, its presets are copied over -- with the lists an
  older release kept there for the window, which it takes up as tabs once
  (ADR-0259) -- made beside it and renamed into place; the engine's database
  is left as it was.
- In Qt's test mode Melody's data, like the engine's state, is the test's
  own folder (`MELODY_DATA_DIR`), so no test shares presets with another.
- **No engine keeps naming layouts.** `layouts.list` and `layouts.put` are
  gone, and so is copying them to engines. A request that names files
  carries its layout: `convert.apply` takes `layout: {name?, directory,
  basename}`.
- **melody-cli reads Melody's presets** on its computer: `convert --layouts`
  lists them, `--layout NAME` sends that layout's expressions, and
  `--layout-expression EXPR` names files by a path expression without a
  saved layout (the folders, then the name after the last slash outside any
  function's brackets).
- Move destinations stay as they are for now: this computer's in Melody's
  presets, another engine's on that engine (a later slice).

## Verification

- `list-repository`: presets and the window's old lists taken over once,
  never twice, never over what Melody keeps; the engine's database
  unchanged.
- `bench-main-window`: a layout saved in Settings is kept in Melody's
  presets; the engine has none to list.
- `engine-file-work`: an engine has no `layouts.list` or `layouts.put`.
- `melody-cli`: with no presets there are no layouts to list; a layout
  expression names the converted file.

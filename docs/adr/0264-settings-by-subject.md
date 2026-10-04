# ADR-0264: Settings by subject, every engine's folders in one place

## Status

Accepted, 2026-10-04.

## Context

Settings › Library held this computer's music folders, the ratings written
into files and, lately, how long writes are kept for undo. The ratings and
undo settings are handed to every engine, not this computer's alone, so the
page said one thing and did another. And an engine elsewhere had its music
folders only behind its library's Folders… button, while this computer's
were in Settings: two places for one thing.

## Decision

- **Library** shows the music folders of each engine connected, one at a
  time, chosen at the top (the chooser hidden while there is only this
  computer). Each engine's Folders… opens it there, at that engine.
- **File operations** (once Naming) holds what writing files does, as
  tabs: **Rename & move** (naming layouts, move destinations),
  **ReplayGain** (sidecar only, true peak) and **Undo** (what each engine
  keeps of what its writes replace). ReplayGain is a tab here rather than
  under a tagger heading: it is not tagging alone -- it has its own tool and
  can be kept in sidecar files -- but it is a write to files all the same.
- **Ratings** is a page of its own: ratings written into the files, their
  backup copy, other players' RATING tags -- handed to every engine.
- Covers stays a page of its own.

## Consequences

- One place for an engine's folders, whichever engine.
- `SettingsDialog::Page::naming`, `replaygain` and `undo` open File
  operations at their tab; `file_operations` and `ratings` are pages.
  `SettingsSession::Page` and its titles follow the same rows.

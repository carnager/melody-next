# ADR-0323: Identify in two lines, at the tag editor's size

## Status

Accepted, 2026-10-25. Amends ADR-0311's window.

## Context

The Identify window opened over most of the screen, and its two lists were
tables: the albums as two narrow columns (album, state), cut short to
"Bush ...Stone"; the versions as six columns, the first repeating "Same
album …" on nearly every row and the album and artist on all of them, while
what tells one version from another -- date, country, label, catalogue
number -- was cut short at the far end.

## Decision

- **Two lines to a row**, drawn as the track lists draw a second line:
  - an album: "Artist — Album", beneath it how far it is ("✓ Sixteen
    Stone · CD", "21 versions", "Skipped");
  - a version: "Artist — Album", beneath it how it was found when that says
    something ("Tagged release", "Release named", "same track count",
    "2/2 files"), its track count, media and version. "Same album" itself
    is left out: every version listed is one.
- **The window opens over the tag editor it comes from, at its size**, not
  remembered on its own.

## Verification

- `bench-main-window`: the Identify tests read the rows' two lines.

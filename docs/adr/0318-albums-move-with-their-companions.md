# ADR-0318: An album moves with its companions

## Status

Accepted, 2026-10-25. Takes up the companion-file moves and empty-folder
cleanup ADR-0055, ADR-0057, ADR-0067, ADR-0074 and ADR-0171 deferred. A
folder cover staged with a move is still refused (ADR-0184); written into
the folder an album moves to is the next step.

## Context

Renaming or moving an album with the tag editor moved its audio files and
nothing else: its cover, CUE sheet and playlist stayed in the old folder,
and so did the folder. With covers fetched by Identify (ADR-0316), more
albums have a `cover.jpg` beside them to leave behind.

## Decision

1. **Companions travel with the album.** Where every audio file of a folder
   goes to one other folder -- none left behind -- that folder's images
   (JPEG, PNG, GIF, BMP, WebP, TIFF), CUE sheets and playlists (M3U, M3U8,
   PLS) go with them, keeping their names. They are planned as further
   sources of the path plan, found where the files are (the engine's
   preflight), and checked, journaled, recovered and undone as every moved
   file is. Hidden files, other files and subfolders stay.
2. **A CUE sheet naming a file the plan renames blocks the plan**: moved
   unchanged, the sheet would no longer find its audio. Rewriting the sheet
   is left for later.
3. **The folder an album left goes once empty** -- the album's own folder
   only, never one above it, which may be a library's. Undone, a move makes
   that folder again before the files go back.
4. **A folder cover is refused only where its files move away.** Rename or
   Move ticked, a file that stays where it is gets its folder cover, tags and
   gains written as without them.

## Verification

- `output-path-preflight`: an album leaving whole takes its cover, sheet and
  playlist, not a text file or a hidden image; leaving in part, nothing; a
  sheet naming a renamed file blocks; a cover already at the target blocks.
- `engine-file-work`: an album moved by the engine with its three
  companions, its folder gone; undone, all back in the folder made again.
- `bench-main-window`: Rename ticked, a file already named so, its folder
  cover written with its tags.

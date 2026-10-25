# ADR-0328: An engine takes write plans of thousands of files

## Status

Accepted, 2026-10-25.

## Context

An engine refused any line over a megabyte: "nothing a client asks needs a
megabyte". The tagger's Apply sends its write plan whole --
`preparation.apply`, `artwork.apply` -- naming every file and each change.
124 files identified with MusicBrainz, tags, covers, renaming and moving,
were 1,360,788 bytes: "the message is 1360788 bytes; the engine takes at
most 1048576", and nothing was applied. Covers are not in a plan (they are
uploaded in parts); the plan is the metadata, some 11 KB a file.

## Decision

- **An admitted client may send lines of up to 64 MB**: a plan of several
  thousand files. A peer that has not authenticated keeps the megabyte, and
  is let go past it, as before.
- Past its limit an admitted client is still answered with an error to the
  call it made, and stays connected.

## Verification

- `engine-server`: a 3 MB request is taken; a 65 MB one is refused as the
  call it was, and the connection answers after.

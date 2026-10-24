# ADR-0316: Identify fetches the covers of the albums it applies

## Status

Accepted, 2026-10-25. Extends Identify (ADR-0311) and the cover fetching of
the tag editor's artwork (ADR-0091, ADR-0094, ADR-0184).

## Context

An album identified still had to have its cover fetched by hand: its files
selected, Fetch cover…, the archive's front picked -- for every album, though
the release is known the moment Identify applies it.

## Decision

1. **Fetch covers**, a checkbox in Identify, on by default and remembered:
   once the albums applied are staged, the tag editor fetches each one's
   front cover from the Cover Art Archive, in turn -- the album's files
   selected, the front of the release they now carry (in the draft, as
   Fetch cover… reads it) staged as Fetch cover… stages one chosen. The
   archive's own front is taken, as the picker suggests it. The status says
   for how many albums, and why not for the others.
2. **The artwork reads the files for it, shown or not**: an album's artwork
   is read to stage its cover even when the Artwork page is not open.
3. **A front staged for some files replaces only theirs.** Staging another
   album's front no longer drops the first album's.

Covers follow Cover settings: embedded, beside the files, or both -- and are
written as ADR-0317 says. A batch "fetch missing covers" for the library
could be a plugin.

## Verification

- `bench-main-window`: two albums identified and applied with Fetch covers
  ticked -- each release's listing asked, each file with its album's front
  staged, the status saying both.

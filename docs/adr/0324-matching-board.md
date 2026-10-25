# ADR-0324: Matching on one board

## Status

Accepted, 2026-10-25. Replaces ADR-0265's two panes; keeps its operations.

## Context

Pairing files with a release's tracks was two flat lists side by side,
scrolled together: files on the left, tracks on the right, a "→ 1.1" column
between. Dropping a file on a track worked but nothing said so; the lists
looked like text, not like something to sort. A set whose second disc
repeats the first, split into two albums by its tags, paired both albums
with disc 1 -- and which files had gone where was hard to see.

Matching is mostly checking a suggestion and sometimes mending it: what
disagrees must stand out, and mending must not need a precise drag across a
long scrolling list.

## Decision

1. **One board, a row per album track**: the file on it at the left, the
   track at the right, under a heading per disc ("Disc 2 · CD"); files on no
   track come last under their own heading. One list: the sides cannot
   scroll apart.
2. **Files look movable, tracks stay put.** A file is a raised tile with a
   grip, a hand over it, outlined when picked up; a track is a recessed
   slot; a track without a file has a dashed empty slot beside it.
3. **The line between says how well a pair agrees**: quiet when it does;
   amber, with the difference ("−20 s"), and the row warm, when the file's
   length is more than three seconds from the track's.
4. **Three ways to move a file**, each said under the board before it
   happens ("1.flac → 2.1 · One, swapping with 2.flac"): dropped on a track
   it goes there (a file there takes its place); dropped between files it
   goes there and the files below move down a track; dropped below the
   tracks it is on none. Or clicked -- picked up -- and then its track
   clicked; Esc puts it back. Alt+Up/Down and U stay.
5. **A disc with none of the album's files offers to take another disc's**
   ("Take the files of disc 1"), track for track, in one go.
6. **Two albums of one Identify, the same release**: the later one pairs
   with the tracks the earlier left -- a file aligned to a track the other
   album has goes to one of the same title not taken, and gaps are filled
   with untaken tracks first. A set whose discs repeat each other, split in
   two by its tags, comes out on its two discs.

## Verification

- `bench-main-window`: a file dropped on disc 2's track goes there and its
  length difference is marked; a file clicked and then a track clicked
  goes there; an empty disc takes the other's files by its heading; a
  second album of the same release pairs with the disc the first left; the
  Identify and unequal-count matching tests on the board.

# ADR-0330: Matching as Picard does, coloured by fit

## Status

Accepted, 2026-10-25. Replaces ADR-0324's layout (one board, files beside
tracks); keeps its operations, its drag and click, its disc offer.

## Context

ADR-0324's board kept every file in a row beside a track, unplaced files
under the tracks; whether a pair was right showed only as an amber line
when lengths differed by more than three seconds. Picard puts the release's
tracks at the right, each with its file, the files on no track at the left,
and colours each pair by how well it fits -- the layout the person asked
for once they saw it.

## Decision

- **Right: the release's tracks**, headed by disc, one row each: number,
  title, length, a dot, and the file on it -- a tile to pick up -- or an
  empty slot to drop one in.
- **Left: the files on no track**, gone from there once placed, under the
  name of the folder each is in -- "CD 2", as the tracks under their disc;
  two folders of one name by their parents' too ("Gilded/CD 1"). "Every
  file is on a track" when it is empty.
- **The dot is the fit**, as Picard colours it: green when length and title
  agree (lengths within three seconds, titles at least 80 % alike -- the
  clear-match rule -- or one of them sure and the other unknown), red when
  the length is more than fifteen seconds off or the titles hardly alike,
  orange between. An untagged file's title is its name, without its track
  number. Off rows are tinted their colour; a far-off length is coloured.
- **The heading says it in words**: "All 11 fit", "9 fit · 1 worth a look ·
  1 likely wrong".
- **Moves**: a file dropped on a track goes there (the one there back to the
  left); a file on a track dropped on another swaps with it; dropped at the
  left it is on none. Or picked up by a click and its track clicked; Esc
  puts it back. Alt+Up/Down and U stay.

## Verification

- `musicbrainz-matching`: pairs rated by length and title.
- `bench-main-window`: a far-off pair is called likely wrong and counted in
  the heading; a file dropped at the left is on no track, and put back by a
  click on it and on its track; the unequal-count, Identify and disc tests
  on the new panes.

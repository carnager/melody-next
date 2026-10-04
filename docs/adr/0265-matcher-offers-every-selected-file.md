# ADR-0265: The matcher offers every selected file

## Status

Accepted, 2026-10-04. Amends ADR-0261 (Identify albums…: grouping and
review).

## Context

Identify albums… groups the selection into albums by MusicBrainz release id,
else by tags, else by folder. A file whose tags disagree with its album --
one track of "Suffer" carrying another release's id -- is grouped as an
album of its own, and the album's matcher never lists it: the track it
belongs on stays a gap, and nothing in the window can put it there. Split
and Merge work on whole albums, not on a file.

## Decision

- **The matcher lists every selected file** whose album's grouping can
  still change (not staged, not written): the album's own first, then the
  other albums' -- those sharing a folder with it first -- shown quieter,
  each saying the album it stays in. They are never placed by the
  suggestion and do not count as the album's files.
- **A file put on a track joins the album.** When the pairing is staged,
  a file taken from another album leaves it: that album is looked up again,
  or, left with no files, goes from the list as a merged one does.
- **Files are placed by dragging onto a track.** A file dropped on a
  MusicBrainz track fills it when it has no file, and otherwise changes
  places with the file there; one dropped on a gap in the files pane fills
  that gap, nothing else moving. Dragging between rows still inserts.

Grouping is unchanged: a release id that differs is evidence of two
releases, and the person decides; the matcher is where they can.

## Consequences

- One stray file is fixed where it shows: in the review of its album.
- A large selection lists many files under each album's matcher, after its
  own; the album's own pairing is unaffected, since only its own files are
  aligned.

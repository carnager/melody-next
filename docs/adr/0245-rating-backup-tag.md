# ADR-0245: Ratings can also be copied into a backup tag

## Status

Accepted, 2026-09-30. Extends ADR-0237 stage 2 (ratings in files).

## Context

With **Also write track ratings into the files** on, each engine writes a
track's rating as `FMPS_RATING` -- and on MP3 as Windows Media Player's POPM
-- where other players read it. Those are also the fields other players
*write*: some rewrite POPM on their own byte scale, some clear or re-sync
`FMPS_RATING`. A user moving between players can lose ratings that way, and
asked for a failsafe: a second copy in a tag of their choosing, in a form no
player reinterprets.

## Decision

- Under the option, **Also write to a backup tag:** with a name, by default
  `TRACKKNIFE_RATING`. While ratings are written, each rated track's files
  also carry its rating as the plain 0–10 number in that tag; unrated removes
  it, as it removes `FMPS_RATING`, so "unrated" and "never rated" agree.
- The name is free. An official tag -- COMMENT, TITLE, GENRE... -- is written
  as each format writes it (COMMENT as MP3's COMM frame), and Settings warns
  that whatever it holds is replaced in every rated file. Any other name is
  written exactly as spelled: a Vorbis field, `TXXX:<name>` in MP3, a freeform
  atom in MP4. A name no format can carry (not plain ASCII, containing `=`) or
  `FMPS_RATING` itself is refused, and nothing is copied.
- The copy is written, never read: `FMPS_RATING` and POPM stay what the library
  takes from files, so there is no rule to invent for when the two disagree.
  It is brought back deliberately, by a tagging script's **Convert rating to
  FMPS_RATING** step on the 0–10 scale (ADR-0244).
- The setting is the engines', as ratings-in-tags is: kept as engine state
  `ratings.backup-tag` (empty is off) and set through `ratings.set_tags
  {backup_tag}`. Naming or renaming it while ratings are written writes every
  rated track once, so existing files get the copy too; a tag left behind by
  a rename stays in the files, as ratings do when writing is turned off.
- The checks -- which names can hold the copy, which are official -- are one
  implementation in `metadata/ratings`, used by the engine and by Settings.

## Consequences

- A rating survives any player that only knows `FMPS_RATING` or POPM.
- Choosing COMMENT costs the files their comments, knowingly; the default
  costs nothing.

## Validation

`tests/engine_file_work_test.cpp` writes a FLAC with the copy under its own
name, adds a copy under a new name to an already-rated file, writes MP3's COMM
frame for COMMENT, removes both on unrated, and checks the name rules.

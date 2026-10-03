# ADR-0261: Identifying many albums at once

Status: Accepted (2026-10-03)

Extends the MusicBrainz identification of ADR-0089, ADR-0090, ADR-0158 and
ADR-0162, which identify one album: the files selected are matched to one
release, aligned, and staged.

## Context

Identifying a library is identifying many albums. Today that is one album at
a time: select its files, search, choose the release, align, stage, apply,
and select the next -- dozens of separate searches, previews and journals,
and nowhere to see how far the batch has come. Most albums need no
judgement at all: the files already carry a release id, or one release fits
them exactly. The time should go to the few that do.

MusicBrainz answers one request a second, so a few thousand albums take
hours: the batch has to keep going while the person works through it.

## Decision

**Identify albums…** works on any selection -- a few albums or a kept search
of the library -- in four steps, in one window. Mockups:
https://claude.ai/artifact/1LtLajSJ9KNDJdZZxHvQ2A

**1. Group.** The selection is split into albums, each saying how it was
grouped: by the MusicBrainz release id its files carry; else by album artist,
album and date across folders, so `CD 1` … `CD 6` folders are one album; else,
for untagged files, by folder. A group can be split by folder, merged with
another, or left out before anything is looked up -- a wrong group is the
costliest mistake to undo later.

**2. Look up.** Every group is looked up in the background, at MusicBrainz's
pace, with progress and the time left: a group with a release id by that id,
without a search; the others by search, ranked by track count, lengths and
titles, with AcoustID for untagged ones when configured (ADR-0096). Answers
are cached, so a second run is cheap. Each group says where it is:
waiting, searching, matched with a confidence, needs a choice, no match,
staged, skipped; the list can be narrowed to any of these. The person can
review while the lookup runs.

A **clear match** -- as many files as tracks, every length within a few
seconds, titles alike, pairing by track number without gaps -- is staged by
itself and says why it matched and what it would change. Anything else
waits for the person.

**3. Review.** One group at a time, only those that need it, in the matcher
of ADR-0162, with the release versions that fit above it and their scores.
Driven from the keyboard: Enter accepts and moves to the next, S skips, ↑/↓
choose another version, Alt+↑/↓ move a file, U leaves a file unmatched.
**Next needing you** jumps there from anywhere.

**4. Apply once.** Every staged album lands in one draft, previewed whole:
each album with its release, what changes and its state, each with its own
checkbox. One write, journaled, with one undo; each file is read again
first, and one that changed since the lookup is left out and said so,
without stopping the rest. Albums skipped or unmatched stay in the list for
later. In the same write, each chosen on its own, all off by default:

- **Rename the files** by a naming preset, in the folders they are in;
- **Move into folders** by a folder preset, under the library folder they
  are in;
- **Scan ReplayGain**: track and album gain, or track gain only; **skip
  albums that already have gain**, on by default. Scanned after the tags are
  written.

The presets are the naming layouts the file tools already use (and copy to
each engine); **Edit naming presets…** opens them.

## Consequences

- One preview and one undo for a batch of any size; the review goes where
  judgement is needed.
- Grouping, lookup and staging are new; the matcher, MusicBrainz client,
  cache, pacing, naming layouts, the write plan and its journal, and the
  ReplayGain scan are those there are.
- A long lookup belongs to the open window: closing it stops the lookup, and
  the cache makes starting again cheap. Keeping a batch across a restart is
  **Unknown** until wanted.

## Not decided here

- The thresholds of a clear match, beyond the rule above: measured on real
  albums first.
- Writing a batch's changes in parts, should one very large batch prove too
  slow to preview whole.

## Verification

- Grouping: by release id, by tags across disc folders, by folder for
  untagged files; split, merge and leave out.
- Lookup: a release id is looked up without a search; pacing holds at one
  request a second; a clear match is staged with its reasons, an ambiguous
  one is not.
- Review: accept, skip, other version and next-needing-you from the keyboard.
- Apply: one plan for every staged album; a file changed since the lookup is
  left out and reported, the rest written; rename, move and ReplayGain each
  only when chosen, with the preset chosen; skip-existing leaves albums with
  gain alone; one undo restores the batch.

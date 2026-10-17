# ADR-0289: Undo while the tagger is open

## Status

Accepted, 2026-10-17. Changes ADR-0263's retention: what a write replaced is
kept only for the tagger that wrote it, while it is open. ADR-0263's undo
itself, ADR-0266's undo copy places and every write's journal stay.

## Context

- **Every write kept the file it replaced** -- a tag write, a move, a rating
  copied into the files, a ReplayGain scan -- within ADR-0263's limits (7
  days, 256 writes, 10 GB), which the engine applied at its start.
- **A rating sweep wrote 35,000 files** on a friend's engine, over SMB on
  macOS. At its next start the engine opened and locked each of those
  sources on the NAS to check it, then released all but 256, one at a time,
  before it answered anyone: it was still silent after nine minutes. A file
  another SMB client held locked would have kept it waiting forever.
- **Nothing undid most of them.** Only Identify albums… offered undo; a
  rating or a ReplayGain scan is written again from the library, not
  undone. No other tagger keeps any of this: it is the person's to look at
  what was written.

## Decision

1. **The safe write stays.** Every write is still a prepared copy published
   whole, journaled, finished or rolled back after a crash (ADR-0044 and
   after). Only what is kept after it changes.
2. **A write keeps nothing unless a tagger asks.** `metadata.apply`,
   `artwork.apply` and `preparation.apply` take `keep_undo`; without it, the
   file a write replaced is let go of as soon as the write is done. Ratings
   written into files and ReplayGain scans never ask. A CUE sheet's and a
   sidecar's never are kept: no tagger writes them.
3. **The tagger keeps them while it is open.** It writes with `keep_undo`,
   stays open after a write -- the files read again, what it said kept --
   and offers **Undo write**, newest first, one write at a time. Closing it
   lets go of every copy it kept (`backups.release`); Identify albums…
   hands its own to the tagger it wrote from, and takes back those it
   undoes.
4. **Left ones go at the next start, after clients are answered.** A copy a
   tagger never let go of -- it crashed, the engine stopped -- is listed
   before the engine answers and deleted on a thread of its own after.
   Nothing is checked but the copy itself before it is deleted; no source is
   opened, and no limit is kept.
5. **The limits go:** `backups.retention`, `backups.set_retention`, Settings
   › File operations › Undo's days, writes and gigabytes, and
   `maintain_metadata_backups` / `maintain_publication_backups`. The Undo
   page says what is kept and keeps ADR-0266's where.

## Consequences

- A write costs no space after it is done, except while the tagger that made
  it is open.
- Undo after the tagger closed, or after a restart, is gone: look before
  closing.
- An engine starts at once however many writes it made before.
- An engine older than this keeps its limits; a client older than this
  writes without `keep_undo` to a newer engine and gets no undo -- which it
  only ever offered in Identify albums…. Protocol level 16.

## Verification

- `engine-file-work`: a write kept is listed, let go of when asked and at
  the next start; one not asked to keep keeps nothing; undo through the
  engine with `keep_undo`.
- `bench-main-window`: the tagger written from stays open with Undo write;
  Undo write puts the file and the row back; closing lets go of the copy.
- `metadata-commit`, `file-publication`: a released backup is gone,
  copied, moved or kept beside alike.

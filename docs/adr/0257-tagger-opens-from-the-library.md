# ADR-0257: The tagger opens from the library, and reads the files behind it

Status: Accepted (2026-10-03)

## Context

Opening the tagger on a selection reads every file's tags first: they are the
baseline a write starts from, and the cached tags a list row carries are a
presentation snapshot -- canonical names and values, without the native
field names, frame layout or provenance a write has to preserve (AGENTS.md:
"Preserve unknown metadata").

On gemenon a file the disk has not cached takes about 25 ms to read, almost
all of it the disk finding the file (measured 2026-10-03: 0.2--0.4 ms once
cached). Reading in batches with the engine reading eight files at a time
brought that to 8.4 ms a file -- 66,000 files in about nine minutes, with
progress and Stop. Faster than the 31 minutes before, but the tagger is
still a long wait before it shows anything, for a selection the engine's
library has already described.

What makes a different order possible: **saving reads every file again
anyway.** The write plan re-reads each source, refuses one whose revision
moved since its baseline (`source_changed`), and merges the staged edits into
what it just read -- so the cached snapshot can be looked at, and value edits
staged on it, long before it would be safe to write from.

## Decision

**The tagger opens at once on what the list already holds.** A row from the
engine's library comes with its cached tags; the grid is built from them
straight away, *provisional*: shown, sortable, open to value edits, and never
the baseline of a write.

**The files are read behind it** -- batched, eight at a time on the engine --
with "Reading tags · N of M · Stop" in the status line.

**When every file is read, the selection is rebuilt once** from the files'
own tags, and the grid takes it in place. Drafts staged meanwhile carry over
by row and field name: a value edit is the same edit whatever the old value
was. The undo history from before the switch is dropped; the drafts stay,
and the status says the tags were read.

**Until then, what depends on the real tags waits:**

- *Value edits* -- typing a value, clearing or adding a field, replacing a
  field's values across the selection -- are open at once.
- *Derived edits* -- scripts and automatic transformations, find and replace,
  case changes, suggestions, a MusicBrainz match, the tagger's ReplayGain
  scan (staged through the same path) -- and *Save* are disabled, saying they
  wait for the tags to be read. The window's own ReplayGain dialog is
  unaffected: it reads before it measures, with progress.

**Batch by batch was considered** -- each batch's real tags replacing its
rows as they come, derived edits and Save waiting only for the rows they
touch. Rejected for now: the selection's common and mixed field states, the
grid and its undo history would all have to change under edits in progress,
in the most delicate part of the tagger, for a wait that is the same for a
save of the whole selection.

**Not a cache as a baseline.** Rejected: the snapshot lacks native names and
layout, so a plan built on it could not show completely what a write would
do, and AGENTS.md requires a complete preview. Here the snapshot is only ever
shown, never written from.

**Rows not from the engine's library** -- dropped files, folders, rows with
no cached tags -- are read before they are shown, as before.

## Consequences

- The tagger shows a selection of any size in the time its rows take to
  collect; reading never blocks looking or typing.
- A save, or a script, may wait for reads, with progress; never longer than
  opening waits today.
- The grid's values can change once while it is open, when real tags replace
  cached ones. For an up-to-date library they are the same values.

## Verification

- A selection of library rows opens before any file is read, provisional;
  the read then rebuilds it from the files' own tags.
- A value edit staged before the switch survives it, on the same row and
  field; derived edits and Save are refused until it.
- A plan is never built from a provisional selection.

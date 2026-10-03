# ADR-0257: The tagger opens from the library, and reads the files behind it

Status: Proposed (2026-10-03)

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

Two facts make a different order possible:

- **The library records each file's exact state when it was indexed**
  (`local_library_tracks.revision`: device, inode, size and modification time
  to the nanosecond) -- the same `LocalSourceRevision` the tagger captures
  and the write plan checks.
- **Saving reads every file again anyway.** The write plan re-reads each
  source, refuses one whose revision moved since its baseline
  (`source_changed`), and merges the staged edits into what it just read --
  so what is written never depends on the cached snapshot's completeness.

## Decision (proposed)

**The tagger opens at once from the library.** A row from the engine's
library carries the revision it was indexed at. The grid is built from the
cached fields, each row marked *provisional*: shown, editable, not yet the
baseline of a write.

**The files are read behind it.** The real reads run as they do now --
batched, eight at a time on the engine -- and each batch replaces its rows'
provisional baselines with the files' own tags. The grid updates in place;
the status line says "Reading tags · N of M". A file whose real revision
differs from the indexed one changed since the library last saw it: its
row takes the real tags, and it is said so, once, in the status.

**What an edit may use depends on what it reads.**

- *Value edits* -- typing a value, clearing a field, replacing the values of a
  field across the selection -- do not depend on the old value and are staged
  at once on provisional rows.
- *Derived edits* -- scripts, transformations, case changes, find and replace,
  anything computed from a current value -- run only on rows that have been
  read. Asked of provisional rows, the reads of exactly those rows are moved
  to the front, and the edit runs when they are in, with progress.
- *Edits staged on a row before its real tags arrive* stay: a value edit is
  the same edit whatever the old value was. A row whose file changed since
  indexing keeps its staged value edits and loses any derived ones, said so.

**Save waits for what it writes.** A write plan is built only from read rows.
Saving with edits on provisional rows moves their reads to the front and
saves when they are in; Stop leaves the drafts as they were.

**ReplayGain needs no baseline to measure.** A scan starts at once on the
files; their reads run alongside, and writing the gains waits for them like
any other save.

**Not a cache as a baseline.** Rejected: the snapshot lacks native names and
layout, so a plan built on it could not show completely what a write would
do, and AGENTS.md requires a complete preview. Here the snapshot is only ever
shown, never written from.

## Unknown

- `StagedMetadataSelection` is immutable once made; replacing an item's
  baseline needs an operation that rebuilds that item's cells and the field
  states it touches, without disturbing staged patches. Its cost on 66,000
  rows, applied a batch at a time, has to be measured.
- Whether provisional rows should look different in the grid. **Proposal:**
  no per-cell styling -- the status line carries it -- because 66,000 rows
  turning from one look to another over nine minutes is noise.
- Rows that are not from the engine's library (dropped files, folders) have
  no indexed revision: they are read before they are shown, as now.

## Consequences

- The tagger shows a selection of any size in the time its rows take to
  collect; reading never blocks looking or typing.
- A selection's first save may wait for reads, with progress; never longer
  than opening waits today.
- The grid's values can change while it is open, as real tags replace
  cached ones. For an up-to-date library they are the same values.

## Verification

- A selection of library rows opens before any file is read; the reads then
  replace every baseline, and a file changed since indexing is flagged.
- A value edit staged before a row's read survives it; a derived edit asked
  of provisional rows waits for exactly those rows.
- Save of edits on provisional rows reads them first; Stop leaves the
  drafts.
- A plan is never built from a provisional baseline (asserted in the
  planner).

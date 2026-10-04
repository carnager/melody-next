# ADR-0266: Undo copies where the person says, restored without an exchange

## Status

Accepted, 2026-10-04. Amends ADR-0263 (what was written can be undone), and
the backup placement of ADR-0111/0115 and ADR-0237.

## Context

Every write keeps what it replaced: a tag write a backup of the file, a move
that writes a new file its source. Both were kept beside the file, as
hidden `.trackknife-<id>.metadata-backup` and `.trackknife-<id>.retained`
files in the album's folder -- for ADR-0263's seven days, and before it
until the engine next started. A library on a NAS had them for days in
album folders, and everything that copies those folders took them along: a
shared album's archive twice its size, and a remote backup holding a full
second copy of every file tagged that week. Copies whose journal records
were lost (a restored workspace) stayed for good.

And undo restored by exchanging the file and its backup atomically
(`RENAME_EXCHANGE`). NFS has no such exchange: on a NAS mounted over NFS,
"Undo this batch" refused every file.

## Decision

- **Where undo copies are kept is the person's choice, per engine**
  (`backups.location`, set from Settings › File operations › Undo, handed
  to every engine as the retention is):
  - **the engine's folder** (default): `undo/` in the engine's state
    folder, outside every library;
  - **a folder** the person chooses, anywhere -- a library folder too: it
    is their choice, said beside it, not refused;
  - **beside each file**, as before.
- **A copy is a hard link where it can be**, else a verified full copy:
  the engine tries the link, and a folder on another filesystem answers
  with a refusal, not a guess. On the same filesystem a kept copy costs no
  space while the file is unchanged; across filesystems it is the file's
  size, counted by the retention's size limit.
- **A write leaves nothing beside the file** but its prepared copy while it
  runs (the one rename that publishes it must stay in the folder): the
  backup is made where the person keeps undo copies, then the prepared file
  is renamed over the original. A move's retained source is moved there
  too -- renamed on one filesystem, else copied, verified and removed.
- **Restore needs no exchange.** Undo, and a failed write's rollback, put
  the original back by renames within the file's own folder: the original
  copied (or linked) beside it from where it is kept, the current file
  renamed aside, the original renamed into place, the library told, the
  set-aside file removed. Each step is journaled; a start after a crash
  finishes an undo half done, or puts the file back. Undo therefore works
  on NFS and SMB.
- **Copies already beside files** that an engine has records of are moved
  where it keeps undo copies at its start; copies without records are not
  the engine's to judge, and are left for the person.

## Consequences

- Album folders hold only music, by default.
- A library on another filesystem than the engine's folder pays a full copy
  per write, read over the network for a NAS; choosing a folder on the NAS
  makes it a link again.
- Each file's undo briefly has two files beside it -- the original being
  put back and the one set aside -- as a write has its prepared copy.

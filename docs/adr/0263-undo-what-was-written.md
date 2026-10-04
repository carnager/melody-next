# ADR-0263: What was written can be undone

## Status

Accepted, 2026-10-04. Completes ADR-0261's "Undo this batch"; changes the
backup policy of ADR-0237's engine and the publication lifecycle of
ADR-0111/0115.

## Context

Every tag write keeps the file it replaced (a metadata backup), and undo
restores it whole -- for every container the commit writes, as a test now
proves byte for byte. But nothing offered undo: the engine released every
backup at its next start, and no client could ask for one.

Moves were worse. A same-filesystem rename keeps the file and can be renamed
back. But a publication that writes a new file at its target -- a move to
another filesystem, or tags and path changed together, the usual write of
Identify albums… -- deleted its source once the target was published. Its
bytes were gone: nothing could be undone.

## Decision

- **A publication never deletes its source.** Where it would, the source is
  renamed beside itself (`.trackknife-<journal id>.retained`, a no-replace
  rename on its own filesystem), and the publication's retained source is
  recorded when it completes (`file_publication_backups`, schema 52), with
  the lifecycle a metadata backup has: retained, undoing, undone, released,
  or left for reconciliation. A same-filesystem rename keeps the file itself
  and records nothing.
- **Undo restores, never guesses.** A retained publication is undone only
  when its target is still what was written, its retained source still the
  original and the source path free: the source renamed back, the target
  removed, the directories the publication made removed when empty, and
  the catalogue following the file back. Anything else is refused, nothing
  touched, and the record marked for reconciliation. An undo a crash
  interrupts is finished at the engine's next start.
- **Kept, within limits.** Backups -- metadata and publication alike -- are
  kept newest first, up to 7 days, 256 operations and 10 GB each, and the
  rest released at the engine's start (`MetadataBackupRetentionPolicy`
  defaults). An undo is possible while its backups are kept.
- **The engine undoes; the client asks.** The engine gains a method to undo
  operations by journal id; Identify albums… remembers the journal ids of a
  Write and offers **Undo this batch**.

## Consequences

- A write costs the space of the files it replaced until retention releases
  them -- for a batch, about the size of its albums.
- A hidden `.trackknife-*.retained` file sits beside a moved file's old
  place until released; it has no audio extension, so libraries do not list
  it.
- The protocol level rises with the undo method (ADR-0260).

## Progress

- Kept sources (2026-10-04): publications that write a new file retain their
  source; undo, release, retention and recovery of retained sources
  (`undo_retained_publication`, `undo_file_publication`,
  `release_publication_backup`, `maintain_publication_backups`,
  `recover_publication_undos`); the engine applies retention to both kinds
  of backup at its start instead of releasing them all, and finishes
  interrupted undos. Tests on real files: kept byte for byte, undone with
  target and folders gone, refused when the target changed, an interrupted
  undo finished, retention releasing, across filesystems the same.
- The engine undoes (2026-10-04): the `operations.undo` job takes writes by
  kind and journal id and undoes them in the order given
  (`operations::undo_operations`), the library following each file back,
  its own tags re-read where the write had changed them; one refused does
  not stop the rest. `RemoteFileWork::undo` asks for it. Protocol level 2.
- Undo this batch (2026-10-04): a Write remembers what it wrote; the
  window stays after it and offers **Undo this batch**, which undoes it
  newest first through the engine, the lists, library and Up Next following
  each file back as after a write, the tagger reading the files again, and
  the albums staged again -- with the pairing the person confirmed -- ready
  for another Write. Opened from a track's menu there is no tag editor to
  fall back on, so the undo belongs to the window.

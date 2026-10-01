# ADR-0248: A tag save's backup is a copy where the filesystem refuses hard links

## Status

Accepted, 2026-10-01. Extends the native transaction of ADRs 0043–0047 and
ADR-0111's limited-filesystem tolerance.

## Context

Before a tag save publishes its prepared copy over the source, it keeps the
original as a backup beside it, `.trackknife-<id>.metadata-backup`, made with
`link()`. That backup is the original inode: rollback, crash recovery, undo
and retention all recognise it by the source's recorded identity
(`expected_revision`).

Several filesystems refuse hard links outright: SMB on macOS and Linux CIFS
without unix extensions (`ENOTSUP`), FAT and exFAT (`EPERM`), FUSE mounts that
do not implement `link` (`ENOSYS`). On them every save failed with *creating
metadata backup failed: Operation not supported* -- reported from a macOS
engine writing to a NAS over SMB. The prepared copy, the verification and the
publication itself all worked there; only the backup could not be made.

## Decision

- **Hard link first.** Where `link()` succeeds nothing changes.
- **Otherwise a copy.** When `link()` fails with `ENOTSUP`, `EOPNOTSUPP`,
  `EPERM`, `ENOSYS` or `EMLINK`, the backup is a copy of the locked source at
  the same path: created exclusively, written from the locked descriptor,
  given the original's ownership, permissions and extended attributes, synced,
  then given the original's access and modification time (by path, after the
  close, which is when an SMB mount keeps a time; where the filesystem refuses
  it -- sshfs -- the copy keeps its own). It is then read back and compared
  byte for byte with the source, one byte past the end included. Any other
  `link()` error still fails the save as before.
- **The copy's identity is journaled before publication.** A copy is never the
  original inode, so its own revision is recorded in the operation journal
  (`backup_revision`, migration 49: five nullable `backup_*` columns) by a
  `prepared → prepared` transition, the only one that carries it, allowed once.
  Empty means a hard link. Publication waits for that record.
- **One identity for the backup, everywhere.** `backup_identity(record)` is the
  copy's revision when recorded and `expected_revision` otherwise. Every check
  that recognises the backup -- commit, rollback, crash recovery, undo
  admission and exchange, release, retention -- uses it. A source restored
  from the backup carries that identity too, so undo reports it as the
  published revision and verifies the restored content against it.
- **Crash between copy and record.** The source is then untouched and nothing
  was published: recovery removes the unrecorded copy at this operation's own
  path, as it already removes an unrecorded prepared copy.

## Consequences

- Saves work on SMB, FAT/exFAT and FUSE mounts without hard links, keeping
  the property the backup exists for: the source path always names a complete
  file, and a failure restores the original bytes.
- On those filesystems a save writes the file twice and reads both twice
  more; for network mounts that roughly doubles the time of a large batch.
  Filesystems with hard links pay nothing.
- A file restored from a copy is a new inode; the library follows it as a
  changed revision with the original content.
- Undo still needs an atomic exchange of directory entries
  (`renameat2(RENAME_EXCHANGE)`), unavailable on these mounts and on macOS; it
  reports itself unavailable there, as before.
- Folder images (ADR-0184) publish through their own path, which also links
  and exchanges; it is not covered here.
- Tests force the copy with `use_copied_metadata_backups_for_testing()` and
  cover rollback, commit, undo, retention, and recovery before and after the
  copy is journaled.

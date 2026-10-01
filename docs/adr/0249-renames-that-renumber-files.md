# ADR-0249: A file renamed into place may come back with a new inode number

## Status

Accepted, 2026-10-01. Narrows the identity checks of ADRs 0043–0047 and
ADR-0248 on filesystems whose renames renumber files.

## Context

Trackknife recognises a file it is mutating by device, inode, size and
modification time. A tag save renames its prepared file over the source and
checks that what now stands there is the file it renamed; a rollback renames
the backup back and checks the same; crash recovery recognises a publication
by the prepared file's recorded identity.

The macOS SMB client gives a file a new inode number when it is renamed --
measured on a NAS mount: `ino 17773079393785976330 -> 4951988686954297664`,
device, size and time unchanged, and still so seconds later. There every save
was published, then failed its own check, and its rollback refused for the
same reason: *published metadata source cannot be rolled back from the
recorded identities*. The files kept their new tags, a full-size backup copy
each, and a journal record marked for reconciliation.

## Decision

- **After a rename by Trackknife, the inode is compared only where renames
  keep it.** `core::same_file_after_rename(path, before, after)`: equal, or
  equal in all but the inode on a filesystem whose renames renumber files.
- **The filesystem is asked, not assumed.** Only when an inode did change,
  once per device per process: a temporary file beside the path is created,
  renamed and observed, then removed. Where that cannot be done, an inode
  change counts as it always did. ext4, btrfs, XFS, APFS, NFS keep inodes and
  are checked exactly as before.
- **Where it applies**: the published file after a save's rename, a source
  restored by rollback's rename (and the rollback's own precondition), crash
  recovery recognising a published or rolled-back source, and folder-cover
  recovery. Not undo, whose atomic exchange keeps inodes and is unavailable on
  these mounts anyway; not anything that was not renamed by Trackknife.
- **Observed, then recorded.** The identity seen after the rename is what the
  journal records as published, so every later check -- undo, retention --
  is exact again.

## Consequences

- Saves complete on the macOS SMB client.
- On such a filesystem, a file replaced by someone else between Trackknife's
  rename and its check, with the same size and modification time, would pass
  that check. The published document is still reread and every planned field
  verified before the save counts.
- Records already marked for reconciliation stay so: a terminal state is not
  reopened by an update. Their files are cleaned up by hand.
- Tests answer the filesystem's question with
  `simulate_renumbering_renames_for_testing()`.

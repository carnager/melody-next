# ADR-0175: Consistent workspace backup boundary

## Status

Accepted.

## Context

M10 requires backup and restore for the workspace shared by the MPD and local
authorities. Copying `lists.sqlite` as an ordinary file is not safe while its
WAL connections are active, and a corrupt or unrelated SQLite file must never
be accepted as restore input. Operation recovery journals are part of this
database; a backup must capture one committed database state rather than copy
the main file and WAL independently.

## Decision

The persistence module owns a Qt-free workspace database backup boundary using
SQLite's online backup API. Export creates a new destination exclusively,
never replaces an existing file, includes committed WAL contents, closes the
snapshot, and then reopens it read-only for `PRAGMA integrity_check` and
Trackknife schema inspection. A failed copy or validation removes only the
newly reserved destination.

Restore will be a separate restart-safe application operation. Its input must
first pass this same read-only integrity and schema inspection. The application
must close every database owner before atomically exchanging workspace state;
it must not overwrite the live database from a dialog while workers retain
connections. Ordinary preferences stored outside SQLite will be carried by the
user-facing backup bundle layer rather than smuggled into the database image.

## Consequences

- Backups are consistent while Trackknife remains open and writing through WAL.
- Existing user files cannot be overwritten by backup export.
- Corrupt and non-Trackknife SQLite inputs fail before restore is scheduled.
- The database snapshot export is available from the File menu but is not yet
  the complete M10 backup/restore feature; settings bundling, restart staging, and rollback
  remain required before that feature is marked implemented.

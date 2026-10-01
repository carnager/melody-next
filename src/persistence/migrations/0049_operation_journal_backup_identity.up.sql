-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0248: where the filesystem refuses a hard link, a tag save's backup is a
-- copy with an identity of its own, kept here once it is made. Empty for every
-- earlier operation, whose backup is a link.
ALTER TABLE operation_journal ADD COLUMN backup_device BLOB;
ALTER TABLE operation_journal ADD COLUMN backup_inode BLOB;
ALTER TABLE operation_journal ADD COLUMN backup_size BLOB;
ALTER TABLE operation_journal ADD COLUMN backup_mtime_seconds BLOB;
ALTER TABLE operation_journal ADD COLUMN backup_mtime_nanoseconds BLOB;
UPDATE schema_version SET version = 49;

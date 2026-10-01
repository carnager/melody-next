-- SPDX-License-Identifier: GPL-3.0-only
-- A copied backup's identity is lost; recovery then treats such a copy as
-- an unrecognised path and keeps it for reconciliation.
ALTER TABLE operation_journal DROP COLUMN backup_mtime_nanoseconds;
ALTER TABLE operation_journal DROP COLUMN backup_mtime_seconds;
ALTER TABLE operation_journal DROP COLUMN backup_size;
ALTER TABLE operation_journal DROP COLUMN backup_inode;
ALTER TABLE operation_journal DROP COLUMN backup_device;
UPDATE schema_version SET version = 48;

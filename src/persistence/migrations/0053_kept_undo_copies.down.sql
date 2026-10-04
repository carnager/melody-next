-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE file_publication_backups DROP COLUMN kept_mtime_nanoseconds;
ALTER TABLE file_publication_backups DROP COLUMN kept_mtime_seconds;
ALTER TABLE file_publication_backups DROP COLUMN kept_size;
ALTER TABLE file_publication_backups DROP COLUMN kept_inode;
ALTER TABLE file_publication_backups DROP COLUMN kept_device;
ALTER TABLE file_publication_backups DROP COLUMN kept_path;
UPDATE schema_version SET version = 52;

-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0266: where a publication's retained source is kept, when it was moved
-- where undo copies are, and its identity there.
ALTER TABLE file_publication_backups ADD COLUMN kept_path BLOB;
ALTER TABLE file_publication_backups ADD COLUMN kept_device BLOB;
ALTER TABLE file_publication_backups ADD COLUMN kept_inode BLOB;
ALTER TABLE file_publication_backups ADD COLUMN kept_size BLOB;
ALTER TABLE file_publication_backups ADD COLUMN kept_mtime_seconds BLOB;
ALTER TABLE file_publication_backups ADD COLUMN kept_mtime_nanoseconds BLOB;
UPDATE schema_version SET version = 53;

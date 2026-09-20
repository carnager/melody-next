-- SPDX-License-Identifier: GPL-3.0-only
-- Downgrade refuses folder-image records through the narrower CHECK.

DROP INDEX operation_journal_state;
CREATE TABLE operation_journal_v37 (
    id TEXT PRIMARY KEY NOT NULL,
    kind INTEGER NOT NULL CHECK(kind = 0),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),
    source_path BLOB NOT NULL,
    prepared_path BLOB NOT NULL,
    backup_path BLOB NOT NULL,
    expected_device BLOB NOT NULL,
    expected_inode BLOB NOT NULL,
    expected_size BLOB NOT NULL,
    expected_mtime_seconds BLOB NOT NULL,
    expected_mtime_nanoseconds BLOB NOT NULL,
    prepared_device BLOB,
    prepared_inode BLOB,
    prepared_size BLOB,
    prepared_mtime_seconds BLOB,
    prepared_mtime_nanoseconds BLOB,
    published_device BLOB,
    published_inode BLOB,
    published_size BLOB,
    published_mtime_seconds BLOB,
    published_mtime_nanoseconds BLOB,
    error_code INTEGER,
    error_message BLOB,
    content_kind INTEGER NOT NULL DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 3)
);
INSERT INTO operation_journal_v37 SELECT * FROM operation_journal;
DROP TABLE operation_journal;
ALTER TABLE operation_journal_v37 RENAME TO operation_journal;
CREATE INDEX operation_journal_state ON operation_journal(state);
UPDATE schema_version SET version = 37;

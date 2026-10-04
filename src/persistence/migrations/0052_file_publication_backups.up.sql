-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0263: a publication that writes a new file at its target keeps the
-- source it replaced, renamed beside it; its lifecycle is a metadata
-- backup's -- retained, undoing, undone, released, needs reconciliation.
CREATE TABLE file_publication_backups (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES file_publication_journal(id) ON DELETE CASCADE,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
    undo_id TEXT,
    completed_at_unix_seconds INTEGER NOT NULL,
    updated_at_unix_seconds INTEGER NOT NULL,
    error_code INTEGER,
    error_message BLOB
);
CREATE INDEX file_publication_backups_state_time
    ON file_publication_backups(state, completed_at_unix_seconds DESC);
UPDATE schema_version SET version = 52;

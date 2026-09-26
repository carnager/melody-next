-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0233: lists, the engine's own -- working and saved. Not list_documents, which is
-- Trackknife's workspace and rewritten whole by it: two writers of one table
-- would lose each other's changes.
CREATE TABLE engine_lists (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL,
    -- 0 a working list, closed and gone with its tab; 1 saved, kept until
    -- deleted. Saving a working list is changing this.
    kind INTEGER NOT NULL CHECK(kind IN (0, 1)),
    revision INTEGER NOT NULL CHECK(revision > 0),
    created_ms INTEGER NOT NULL,
    modified_ms INTEGER NOT NULL
);
CREATE TABLE engine_list_items (
    list_id TEXT NOT NULL REFERENCES engine_lists(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    entry_id TEXT NOT NULL,
    raw_path BLOB NOT NULL,
    logical_reference BLOB,
    segment_start_sample INTEGER,
    segment_end_sample INTEGER,
    audio_stream_index INTEGER,
    subsong_index INTEGER,
    duration_ms INTEGER,
    title BLOB NOT NULL DEFAULT X'',
    artist BLOB NOT NULL DEFAULT X'',
    album BLOB NOT NULL DEFAULT X'',
    PRIMARY KEY(list_id, position),
    UNIQUE(list_id, entry_id)
);
-- A file moved is looked up by its old path in every list.
CREATE INDEX engine_list_items_path ON engine_list_items(raw_path);
UPDATE schema_version SET version = 46;

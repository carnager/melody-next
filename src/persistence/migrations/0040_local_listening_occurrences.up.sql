-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_listening_sources (
    observation_hash TEXT PRIMARY KEY NOT NULL,
    source_id TEXT NOT NULL
);
CREATE TABLE local_listening_occurrences (
    occurrence_id TEXT PRIMARY KEY NOT NULL,
    track_hash TEXT NOT NULL,
    played_at_ms INTEGER NOT NULL CHECK(played_at_ms > 0)
);
UPDATE schema_version SET version = 40;

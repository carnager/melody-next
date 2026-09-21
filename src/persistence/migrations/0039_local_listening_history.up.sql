-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_listening_history (
    track_hash TEXT PRIMARY KEY NOT NULL,
    play_count INTEGER NOT NULL DEFAULT 0 CHECK(play_count >= 0),
    last_played_ms INTEGER NOT NULL DEFAULT 0 CHECK(last_played_ms >= 0),
    resume_position_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_position_ms >= 0),
    resume_updated_at_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_updated_at_ms >= 0)
);
CREATE INDEX local_listening_history_last_played
    ON local_listening_history(last_played_ms DESC);
UPDATE schema_version SET version = 39;

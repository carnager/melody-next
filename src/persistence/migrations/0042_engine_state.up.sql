-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0220: the queue belongs to the engine, so the engine has to remember it.
-- A window is a view of what the engine holds, and a view cannot be what
-- brings the queue back after the engine restarts.
--
-- A key/value table rather than a modelled one: what the engine persists is
-- its own state document, versioned in the key ("playback.queue.v1"), and
-- giving it columns would mean a migration every time the engine learns a new
-- mode. Nothing outside the engine reads it.
CREATE TABLE engine_state (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
UPDATE schema_version SET version = 42;

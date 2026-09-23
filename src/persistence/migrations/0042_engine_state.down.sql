-- SPDX-License-Identifier: GPL-3.0-only
-- The engine's remembered queue is derived from what was playing; a downgrade
-- loses it and the engine comes back empty, which is the state it had before
-- the table existed.
DROP TABLE engine_state;
UPDATE schema_version SET version = 41;

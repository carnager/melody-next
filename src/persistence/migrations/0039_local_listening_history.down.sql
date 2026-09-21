-- SPDX-License-Identifier: GPL-3.0-only
-- Refuse to discard listening history on development downgrade.
CREATE TEMP TABLE require_empty_listening_history (count INTEGER CHECK(count = 0));
INSERT INTO require_empty_listening_history SELECT count(*) FROM local_listening_history;
DROP TABLE require_empty_listening_history;
DROP INDEX local_listening_history_last_played;
DROP TABLE local_listening_history;
UPDATE schema_version SET version = 38;

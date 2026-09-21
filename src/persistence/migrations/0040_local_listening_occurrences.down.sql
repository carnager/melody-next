-- SPDX-License-Identifier: GPL-3.0-only
-- Identity links and deduplication evidence cannot be discarded safely.
CREATE TEMP TABLE require_empty_listening_occurrences (count INTEGER CHECK(count = 0));
INSERT INTO require_empty_listening_occurrences SELECT count(*) FROM local_listening_sources;
INSERT INTO require_empty_listening_occurrences SELECT count(*) FROM local_listening_occurrences;
DROP TABLE require_empty_listening_occurrences;
DROP TABLE local_listening_occurrences;
DROP TABLE local_listening_sources;
UPDATE schema_version SET version = 39;

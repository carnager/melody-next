-- SPDX-License-Identifier: GPL-3.0-only
-- Refuse to silently discard user definitions during a development downgrade.
CREATE TEMP TABLE require_empty_saved_searches (count INTEGER CHECK(count = 0));
INSERT INTO require_empty_saved_searches SELECT count(*) FROM saved_searches;
DROP TABLE require_empty_saved_searches;
DROP TABLE saved_searches;
UPDATE schema_version SET version = 31;

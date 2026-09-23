-- SPDX-License-Identifier: GPL-3.0-only
-- Refuse to turn remote lists into local ones on development downgrade.
CREATE TEMP TABLE require_no_remote_lists (count INTEGER CHECK(count = 0));
INSERT INTO require_no_remote_lists SELECT count(*) FROM list_documents WHERE remote != 0;
DROP TABLE require_no_remote_lists;
ALTER TABLE list_documents DROP COLUMN remote;
UPDATE schema_version SET version = 42;

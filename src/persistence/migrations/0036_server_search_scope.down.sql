-- SPDX-License-Identifier: GPL-3.0-only
DELETE FROM saved_searches WHERE scope = 2;
ALTER TABLE saved_searches RENAME TO saved_searches_v36;
CREATE TABLE saved_searches (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 256),
    expression BLOB NOT NULL CHECK(length(expression) BETWEEN 1 AND 4096),
    dialect TEXT NOT NULL CHECK(dialect IN ('tkq-1', 'words-1')),
    scope INTEGER NOT NULL CHECK(scope IN (0, 1)),
    revision INTEGER NOT NULL CHECK(revision > 0)
);
INSERT INTO saved_searches SELECT * FROM saved_searches_v36;
DROP TABLE saved_searches_v36;
UPDATE schema_version SET version = 35;

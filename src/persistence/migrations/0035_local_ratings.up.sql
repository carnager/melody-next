-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_ratings (
    hash TEXT PRIMARY KEY,
    type TEXT NOT NULL CHECK(type IN ('track','album')),
    rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 10),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
ALTER TABLE local_library_tracks ADD COLUMN rating_hash TEXT NOT NULL DEFAULT '';
ALTER TABLE local_library_tracks ADD COLUMN album_rating_hash TEXT NOT NULL DEFAULT '';
CREATE INDEX local_library_rating_hash ON local_library_tracks(rating_hash);
CREATE INDEX local_library_album_rating_hash ON local_library_tracks(album_rating_hash);
UPDATE schema_version SET version = 35;

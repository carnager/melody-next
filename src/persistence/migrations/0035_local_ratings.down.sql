-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX local_library_album_rating_hash;
DROP INDEX local_library_rating_hash;
ALTER TABLE local_library_tracks DROP COLUMN album_rating_hash;
ALTER TABLE local_library_tracks DROP COLUMN rating_hash;
DROP TABLE local_ratings;
UPDATE schema_version SET version = 34;

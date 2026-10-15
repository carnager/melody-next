-- SPDX-License-Identifier: GPL-3.0-only
-- An album's year is its earliest track date, looked up per album when tracks
-- are ordered by it; this index answers that from the index alone.
CREATE INDEX local_library_album_date ON local_library_tracks(album_key, date);
UPDATE schema_version SET version = 55;

-- SPDX-License-Identifier: GPL-3.0-only
-- Entries lose their album artist, date and given ReplayGain; a list played
-- from then on groups and levels by what the files say.
ALTER TABLE engine_list_items DROP COLUMN album_peak;
ALTER TABLE engine_list_items DROP COLUMN album_gain_db;
ALTER TABLE engine_list_items DROP COLUMN track_peak;
ALTER TABLE engine_list_items DROP COLUMN track_gain_db;
ALTER TABLE engine_list_items DROP COLUMN date;
ALTER TABLE engine_list_items DROP COLUMN album_artist;
UPDATE schema_version SET version = 49;

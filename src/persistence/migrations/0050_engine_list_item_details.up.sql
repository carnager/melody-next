-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0256: a list plays by reference, so an entry carries what a queue entry
-- needs and the engine cannot read for itself: the album artist and date the
-- album shuffle groups by, and a ReplayGain from a sidecar or a CUE sheet.
ALTER TABLE engine_list_items ADD COLUMN album_artist BLOB NOT NULL DEFAULT X'';
ALTER TABLE engine_list_items ADD COLUMN date BLOB NOT NULL DEFAULT X'';
ALTER TABLE engine_list_items ADD COLUMN track_gain_db REAL;
ALTER TABLE engine_list_items ADD COLUMN track_peak REAL;
ALTER TABLE engine_list_items ADD COLUMN album_gain_db REAL;
ALTER TABLE engine_list_items ADD COLUMN album_peak REAL;
UPDATE schema_version SET version = 50;

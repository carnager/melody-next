-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE local_library_tracks ADD COLUMN field_index_complete INTEGER NOT NULL DEFAULT 0 CHECK(field_index_complete IN (0,1));
CREATE INDEX local_library_incomplete_fields ON local_library_tracks(raw_path) WHERE available=1 AND field_index_complete=0;
UPDATE schema_version SET version = 33;

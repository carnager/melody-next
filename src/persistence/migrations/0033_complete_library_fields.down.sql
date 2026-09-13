-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX local_library_incomplete_fields;
ALTER TABLE local_library_tracks DROP COLUMN field_index_complete;
UPDATE schema_version SET version = 32;

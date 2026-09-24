-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX local_library_added;
ALTER TABLE local_library_tracks DROP COLUMN added;
UPDATE schema_version SET version = 43;

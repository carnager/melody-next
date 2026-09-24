-- SPDX-License-Identifier: GPL-3.0-only
-- When each track came into the library, in Unix seconds: when a scan first
-- found it, or for a folder's first scan its file's modification time. 0
-- until filled from the modification time when the library next opens.
ALTER TABLE local_library_tracks ADD COLUMN added INTEGER NOT NULL DEFAULT 0;
CREATE INDEX local_library_added ON local_library_tracks(added);
UPDATE schema_version SET version = 44;

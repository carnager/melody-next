-- SPDX-License-Identifier: GPL-3.0-only
-- Retained sources are no longer tracked; the files stay where they are.
DROP INDEX file_publication_backups_state_time;
DROP TABLE file_publication_backups;
UPDATE schema_version SET version = 51;

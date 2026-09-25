-- SPDX-License-Identifier: GPL-3.0-only
DROP TRIGGER local_library_revision_update;
DROP TRIGGER local_library_revision_delete;
DROP TRIGGER local_library_revision_insert;
DROP TABLE local_library_revision;
UPDATE schema_version SET version = 44;

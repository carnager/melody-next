-- SPDX-License-Identifier: GPL-3.0-only
-- Drafts become ordinary working lists, no longer tied to the saved list they
-- drafted.
DROP INDEX engine_lists_draft;
ALTER TABLE engine_lists DROP COLUMN draft_base;
ALTER TABLE engine_lists DROP COLUMN draft_of;
UPDATE schema_version SET version = 50;

-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0259: an unsaved edit of a saved list is a draft on the engine -- a
-- working list naming the saved list it drafts and the revision it began
-- from. One draft to a saved list.
ALTER TABLE engine_lists ADD COLUMN draft_of TEXT;
ALTER TABLE engine_lists ADD COLUMN draft_base INTEGER;
CREATE UNIQUE INDEX engine_lists_draft ON engine_lists(draft_of) WHERE draft_of IS NOT NULL;
UPDATE schema_version SET version = 51;

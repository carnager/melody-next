-- SPDX-License-Identifier: GPL-3.0-only
-- `remote` still says which lists are another engine's.
ALTER TABLE list_documents DROP COLUMN engine;
UPDATE schema_version SET version = 46;

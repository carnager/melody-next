-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0221: a list entry gains an identity independent of its row, so
-- reordering updates position alone. Uniqueness is per document: a copied
-- entry carries its source's identity until the repository stamps it, and the
-- same track legitimately appears in several lists.
--
-- Existing rows are left NULL here and stamped from C++ during migration,
-- because SQLite cannot produce a StableId and randomblob() in a subquery
-- risks folding to one value for every row. NULLs compare distinct in a
-- SQLite unique index, so the index is safe to create first, and the loader
-- treats a NULL identity as "assign a fresh one".
ALTER TABLE list_items ADD COLUMN entry_id TEXT;
CREATE UNIQUE INDEX list_items_entry ON list_items(document_id, entry_id);
UPDATE schema_version SET version = 41;

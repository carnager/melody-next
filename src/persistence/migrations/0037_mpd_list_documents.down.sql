-- SPDX-License-Identifier: GPL-3.0-only
DELETE FROM list_items WHERE document_id IN (SELECT id FROM list_documents WHERE kind = 2);
DELETE FROM list_documents WHERE kind = 2;
UPDATE schema_version SET version = 36;

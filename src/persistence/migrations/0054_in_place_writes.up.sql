-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0281: a write made in place: its backup is the bytes it replaced.
ALTER TABLE operation_journal ADD COLUMN in_place INTEGER NOT NULL DEFAULT 0
    CHECK(in_place IN (0, 1));
UPDATE schema_version SET version = 54;

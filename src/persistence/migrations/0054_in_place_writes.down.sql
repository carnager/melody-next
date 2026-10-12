-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE operation_journal DROP COLUMN in_place;
UPDATE schema_version SET version = 53;

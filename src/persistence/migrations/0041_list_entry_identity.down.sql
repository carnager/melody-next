-- SPDX-License-Identifier: GPL-3.0-only
-- Entry identities are derived state: a downgrade loses them, and the next
-- upgrade stamps fresh ones. Nothing references them yet, so unlike the
-- listening-history downgrade this needs no emptiness guard.
--
-- The index must go first; SQLite refuses to drop an indexed column.
DROP INDEX list_items_entry;
ALTER TABLE list_items DROP COLUMN entry_id;
UPDATE schema_version SET version = 40;

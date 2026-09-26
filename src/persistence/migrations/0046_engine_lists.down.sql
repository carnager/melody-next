-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX engine_list_items_path;
DROP TABLE engine_list_items;
DROP TABLE engine_lists;
UPDATE schema_version SET version = 45;

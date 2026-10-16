-- SPDX-License-Identifier: GPL-3.0-only
INSERT INTO engine_lists SELECT * FROM retired_engine_lists;
INSERT INTO engine_list_items SELECT * FROM retired_engine_list_items;
DROP TABLE retired_engine_list_items;
DROP TABLE retired_engine_lists;
UPDATE schema_version SET version = 55;

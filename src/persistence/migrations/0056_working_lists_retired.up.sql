-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0286: tabs are Trackknife's; only saved playlists stay on the engine.
-- Working lists and drafts are set aside, not deleted -- a client offers them
-- once (list.retired) -- and the down migration puts them back. Foreign keys
-- are off while the schema changes, so the items go by name, not by cascade.
CREATE TABLE retired_engine_lists AS SELECT * FROM engine_lists WHERE kind = 0;
CREATE TABLE retired_engine_list_items AS
    SELECT i.* FROM engine_list_items i JOIN engine_lists l ON l.id = i.list_id
    WHERE l.kind = 0;
CREATE INDEX retired_engine_list_items_list ON retired_engine_list_items(list_id, position);
DELETE FROM engine_list_items WHERE list_id IN (SELECT id FROM engine_lists WHERE kind = 0);
DELETE FROM engine_lists WHERE kind = 0;
UPDATE schema_version SET version = 56;

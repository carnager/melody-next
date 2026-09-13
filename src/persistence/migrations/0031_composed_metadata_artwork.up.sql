-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE operation_journal_artwork RENAME TO operation_journal_artwork_v30;

CREATE TABLE operation_journal_artwork (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES operation_journal(id) ON DELETE CASCADE,
    intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 3),
    target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0),
    original_item_count INTEGER NOT NULL CHECK(original_item_count >= 0),
    planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0),
    original_target_fingerprint BLOB
        CHECK(original_target_fingerprint IS NULL OR
              length(original_target_fingerprint) = 32),
    replacement_fingerprint BLOB
        CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32),
    original_inventory_fingerprint BLOB NOT NULL
        CHECK(length(original_inventory_fingerprint) = 32),
    planned_inventory_fingerprint BLOB NOT NULL
        CHECK(length(planned_inventory_fingerprint) = 32),
    CHECK((intent_kind = 0 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NOT NULL AND
           planned_item_count = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 1 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NULL AND
           planned_item_count + 1 = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 2 AND original_target_fingerprint IS NULL AND
           replacement_fingerprint IS NOT NULL AND
           target_ordinal = original_item_count AND
           planned_item_count = original_item_count + 1) OR
          (intent_kind = 3 AND target_ordinal = 0 AND original_target_fingerprint IS NULL AND replacement_fingerprint IS NULL))
);

INSERT INTO operation_journal_artwork
SELECT * FROM operation_journal_artwork_v30;
DROP TABLE operation_journal_artwork_v30;
UPDATE schema_version SET version = 31;

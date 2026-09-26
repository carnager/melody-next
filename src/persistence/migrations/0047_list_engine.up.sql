-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0234: a list names its engine by the id the engine keeps. Empty is this
-- computer's; the remote's lists say "remote" until that engine has said who
-- it is. `remote` stays, true for any engine's list, for older releases.
ALTER TABLE list_documents ADD COLUMN engine TEXT NOT NULL DEFAULT '';
UPDATE list_documents SET engine = 'remote' WHERE remote != 0;
UPDATE schema_version SET version = 47;

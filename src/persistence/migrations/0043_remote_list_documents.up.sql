-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0227: a list belongs to one engine connection -- this computer's, or
-- the remote one. Its entries are files on that engine's machine, so a remote
-- list opened as a local one would name paths that do not exist here.
ALTER TABLE list_documents ADD COLUMN remote INTEGER NOT NULL DEFAULT 0;
UPDATE schema_version SET version = 43;

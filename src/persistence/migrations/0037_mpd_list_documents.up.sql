-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0181: list documents may use kind 2 (mpd). No DDL change; the bump
-- makes older builds refuse the database cleanly.
UPDATE schema_version SET version = 37;

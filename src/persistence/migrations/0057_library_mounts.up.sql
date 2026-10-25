-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0321: the mounts a library's files were found on, by mount point: what
-- filesystem and from where. A file missing from a folder on the same mount
-- as it was found on is gone; on another -- a share not mounted, its empty
-- mountpoint left -- it is kept.
CREATE TABLE local_library_mounts (mount_point BLOB PRIMARY KEY NOT NULL,
    filesystem TEXT NOT NULL, source BLOB NOT NULL);
UPDATE schema_version SET version = 57;

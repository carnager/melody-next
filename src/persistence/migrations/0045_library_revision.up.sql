-- SPDX-License-Identifier: GPL-3.0-only
-- Whether the library changed since a client last listed it: a counter
-- that goes up when a track comes, goes, or changes what a listing shows,
-- and an id, so a library made anew is not taken for the old one at the
-- same count. A scan that finds nothing new touches every row's seen
-- column and leaves the count alone.
CREATE TABLE local_library_revision (
    id TEXT NOT NULL,
    revision INTEGER NOT NULL
);
INSERT INTO local_library_revision(id, revision) VALUES (lower(hex(randomblob(8))), 1);
CREATE TRIGGER local_library_revision_insert AFTER INSERT ON local_library_tracks
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
CREATE TRIGGER local_library_revision_delete AFTER DELETE ON local_library_tracks
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
CREATE TRIGGER local_library_revision_update AFTER UPDATE ON local_library_tracks
WHEN OLD.title IS NOT NEW.title OR OLD.artist IS NOT NEW.artist OR OLD.album IS NOT NEW.album
    OR OLD.album_key IS NOT NEW.album_key OR OLD.date IS NOT NEW.date OR OLD.disc IS NOT NEW.disc
    OR OLD.track IS NOT NEW.track OR OLD.available IS NOT NEW.available
    OR OLD.duration_ms IS NOT NEW.duration_ms
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
UPDATE schema_version SET version = 45;

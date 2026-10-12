# ADR-0280: Where hard links are refused, the original is moved to be the backup

## Status

Accepted, 2026-10-12. Amends ADR-0248 (copied backup where hard links are
refused).

## Context

A tag write prepares a new copy of the file, keeps the original as the undo
backup, and renames the copy into place. The backup is a hard link to the
original -- free -- but SMB refuses hard links, and there ADR-0248 copies the
original instead, then reads both to compare them. Over a network share that
is two more passes over the file for the backup alone: with the prepared
copy and its checks, a 30 MB track moved about 200 MB per tag edit, and a
library of 18,000 ALAC files took most of a day.

The prepared copy already holds the new file. The original needs no copy:
it can be the backup itself.

## Decision

- **Link, else move, else copy.** The backup is a hard link where the
  filesystem makes one. Where it refuses, the original is renamed to the
  backup's path -- the same file, its bytes untouched, nothing copied -- and
  the prepared copy renamed into its place. Only where that rename cannot be
  made, the undo folder on another filesystem, is the original copied as
  ADR-0248 says.
- **Before the new file is in place, the original is put back.** Every
  failure between the move and the publishing rename renames the backup
  back to the file's name, where a linked or copied backup would be removed.
- **Recovery knows the gap.** A crash after the move and before the publish
  leaves nothing at the file's name and the original at the backup's;
  recovery renames it back and removes the prepared copy.
- The end is the same as a linked backup's: the undo folder holds the
  original, the music folder the new file. Undo and retention are unchanged.

## Consequences

- On SMB a tag write moves the file once less in each direction and skips
  the comparison read: a little under half the traffic of before.
- Between two renames the file's name is briefly empty; a player opening it
  in that moment finds nothing, and recovery covers a crash there.
- Tested: commit, rollback, undo and retention with a moved backup, which
  keeps the original's inode; and recovery of a crash between the renames.

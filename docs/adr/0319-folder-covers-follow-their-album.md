# ADR-0319: A folder cover follows its album

## Status

Accepted, 2026-10-25. Completes ADR-0318; supersedes ADR-0184's refusal of
a folder cover staged with a Rename or Move, but where files move apart.

## Context

ADR-0318 moved an album's companions with it and refused a folder cover
staged in the same Apply wherever its files moved: the cover was planned
for the folder the files were in, and written before they moved. With
Identify fetching covers (ADR-0316), an album identified and sorted into
place in one Apply was refused.

## Decision

1. **The cover is planned for where the album goes.** When all the files a
   folder cover is for move to one other folder, the cover is planned there.
   Where they move apart, it is refused as before.
2. **Written once they are there.** A file that moves writes its tags and
   embedded cover into its new copy; its folder cover is written after every
   file -- and every companion -- has moved, journaled as covers are. The
   cover it replaces is the one that travelled along, kept to undo; a copy
   across filesystems is recognised by its content.
3. **Undone first.** The tagger undoes a moved album's folder cover before
   its moves, so the old cover is back before it moves back.
4. **The folder an album left goes once nothing of it is there**: at the end
   of the Apply, or -- where the file a moved file was is kept to undo it --
   when that copy goes to the undo folder, or is let go of.

## Verification

- `bench-main-window`: an album with a cover moved with a new cover staged:
  the new one in the folder it moved to, the old folder gone once the tagger
  closes; undone, the album and its old cover back in its old folder.

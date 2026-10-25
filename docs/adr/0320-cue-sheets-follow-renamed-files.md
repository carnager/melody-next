# ADR-0320: A CUE sheet follows the files it names

## Status

Accepted, 2026-10-25. Replaces ADR-0318's refusal of a CUE sheet naming a
file the plan renames.

## Context

ADR-0318 moved an album's CUE sheet with it, but blocked the move when the
naming layout renamed a file the sheet names: moved unchanged, the sheet
would no longer find its audio. A single-file rip with its sheet is the
album most often renamed into a layout.

## Decision

1. **The sheet is rewritten as it travels.** Its FILE names that the plan
   renames are written as renamed -- quoted as they were, or where the new
   name has a space -- and every other byte is kept, line endings with it.
   Names it does not rename, and sheets naming none, are moved as they are.
2. **As a prepared copy**, the way a file with new tags moves: journaled,
   recovered, and undone to the sheet as it was. The library, which does not
   index CUE sheets, follows nothing; undone, no tags are read from it.

## Verification

- `output-path-preflight`: FILE names renamed, quoted and not, a CRLF sheet
  otherwise unchanged, a new name with a space quoted, a sheet naming none
  of them left alone; a renamed file no longer blocks its sheet.
- `engine-file-work`: an image renamed as it moves, its sheet rewritten to
  name it, the rest as it was; undone, the sheet and the image as before.

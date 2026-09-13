# ADR-0162: Reorderable MusicBrainz matching

- Status: accepted
- Date: 2026-09-13
- Supersedes: ADR-0158 selected-pair assignment interaction

## Decision

**Trackbench decision:** retain two aligned panes. The left contains local
filenames and lengths; the right contains only MusicBrainz tracks and lengths,
in fixed album disc/track order. Full local paths remain in tooltips. Row position
defines the pairing. Move file up/down and Alt+Up/Down swap local entries;
drag-and-drop inserts a local entry at the chosen position. Album tracks do not
move. Selection follows the moved file, highlights the corresponding right row,
and scrolling is linked so pairings remain aligned.

Paired rows have a shared subtle tint and a bold arrow/track-number marker in
the local pane. Gaps and unmatched files have explicit text labels. These marks
describe assignments, not confidence in the album identification.

Leave unmatched moves a file below the album and leaves an empty slot. Empty
slots can move, allowing missing album tracks without shifting the wrong tags
onto subsequent files. Extra files remain visible below the album and receive
no proposals. Every local occurrence appears exactly once. Only current drags
from this local view are accepted; external, cross-window, and stale drags are
rejected. The widget owns the drag lifecycle to prevent Qt deleting source rows
after the mapping controller has already reordered them.

Initial suggestions retain confident automatic matches and fill remaining
slots in original file order. This fallback is a reviewable ordering suggestion,
not a confidence claim. Match by filename explicitly replaces all pairings with
natural filename order; Reset file order uses original selection order. Neither
operation stages or writes tags. Stage matches confirms the displayed pairing
through the existing core mapping validator and undoable proposal boundary.

## Verification

Identify regressions cover tagged and untagged files, natural filename order,
actual drop-event handling, stale drag rejection, buttons and keyboard movement,
fixed album tracks, synchronized selection, and visible assignment markers.
Unequal-count cases cover missing/extra files across disc boundaries and linked
scrolling, and verify which original occurrences receive proposals. Existing
core mapping tests enforce unique, in-range assignments and omission of
unmatched files.

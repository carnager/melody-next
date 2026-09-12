# ADR-0157: Tag field review controls

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0036, ADR-0041, ADR-0084

## Context

Small tag edits are difficult to review among many unchanged fields. The user
requested usability work after the workspace review. M5 remains active.

## Decision

The local-track action is named **Edit tags…** in the Edit and context menus
to make editing discoverable. It retains **Alt+Return** and opens the existing
**Tags · N tracks** workspace. The editor title and supporting help text use
the same terminology.

**Trackknife decision:** Properties adds a case-insensitive substring filter
over display and canonical field names, a Changed fields only switch, and a
Show files switch. These controls are temporary presentation state. Collapsing
the file selector retains its selection. Filtering never changes a draft,
the selected file scope, or the set of changes written by Apply.

The review bar shows visible/total field counts and the number of fields with
staged edits in the selected files. It explicitly states that Apply includes
hidden edits. Counts reuse the existing background aggregate projection;
changed-only filtering waits for that projection instead of scanning tracks
on the UI thread. Refreshes are coalesced. Changed field names are bold and
have an accessible description in addition to the existing draft colors.

Hidden field rows are deselected and cannot remain invisible Remove/Revert
targets. Add field clears both filters so the new field can be edited.
Source model indexes remain stable; filtering does not introduce another
interpretation of field identity or write semantics.

## Verification

The real-FLAC Properties regression in `bench_main_window_test.cpp` covers
canonical/display name matching, hidden selection clearing, selected-file
scope changes, pending-draft preservation, collapse/expand semantics, undo,
redo, discard, and adding fields while changed-only filtering is active.

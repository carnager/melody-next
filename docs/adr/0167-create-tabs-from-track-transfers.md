# ADR-0167: Create local tabs from track transfers

- Status: Accepted
- Date: 2026-09-13

## Context

Copy to list and Move to list require an existing destination. Users also
expect to create a destination by dropping local tracks on the tab strip.

## Decision

Both local track transfer submenus always offer **New tab…**, including when
there is only one local list. The action requests a name; cancellation or an
empty name leaves the workspace unchanged. Existing destinations follow a
separator. Successful transfers select the new persistent scratch tab.

Dropping local working-list tracks on unused tab-strip space creates a
persistent scratch tab named **Selection** without a modal prompt. Dropping
on an existing local tab appends there. The default is move; Ctrl copies,
matching local track-list drags. Hovering never creates or edits a tab.
MPD tabs, metadata tabs, and the source tab reject these transfers.

Transfers retain occurrence order, duplicates, raw paths, metadata, technical
facts, and captured revisions through the existing row-transfer service.
Already-probed cached rows need no file scan. Unprobed rows continue normal
background preparation in their destination. Move removes source occurrences
only after destination insertion; it never moves files on disk. Existing
list-history behavior remains unchanged.

## Validation

Workspace tests cover named copy/move, cancelled naming, single-local-tab
availability, duplicate/cache preservation, drag hover, Ctrl-copy, default
move, transfers to existing tabs, and rejection of incompatible destinations.

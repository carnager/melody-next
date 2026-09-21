# ADR-0203: Direct tag editing of mapped server files

## Status

Accepted, 2026-09-20. Implements the immediate portion of Task 8.

## Decision

Edit tags on an MPD/Melody selection resolves its URIs through the configured
music-root mapping and opens the existing metadata editor tab directly.
It creates no intermediate local list or queue model. The editor uses the same
sidebar Files page as local-list tag editing; it is not a floating window.
Playback continues while the user edits. ReplayGain, Convert, and explicit Load as
local files retain ADR-0180's materialization path.

Resolve paths lexically with the existing traversal/scheme restrictions. Reject
an unmappable selection before opening rather than silently editing a subset.
Do not stat or probe mapped files on the GUI thread. The editor's existing
cancellable background capture reads actual metadata and source revisions;
missing/inaccessible files leave the editor unavailable for mutation. No server
index metadata is treated as an editable baseline. Apply, preservation, conflict
checks, journals, and dependent local-state updates reuse the existing services.
When no persisted list occurrence exists, dependent-state reconciliation is a
successful zero-row operation and invalidates the optional library scan revision.
The operation journal still owns the commit/recovery evidence; no fabricated
list or positive-occurrence refresh record is created. Existing list occurrences
retain their normal transactional metadata reconciliation.
Main-window close checks both editor tabs and standalone editors for pending
work before stopping background services.

Server-side metadata reads, remote writes, and targeted server library refresh
remain Task 8 follow-ups. This change needs no Melody protocol update.

## Validation

The mapped-action UI regression uses a real FLAC fixture to check complete
metadata/revision capture, file selection hosted in the sidebar, exactly one
editor tab and no new local list, a successful journaled tag write without any local list,
rejected traversal, and asynchronous missing-file failure.
Local editor/sidebar and metadata-write regressions cover the shared factory.

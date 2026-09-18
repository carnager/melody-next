# ADR-0176: Local-list add and replace undo

## Status

Accepted.

## Context

Local list removal, reorder, sorting, reversal, and deduplication already used a
bounded session undo history. Adding tracks cleared that history, while replacing
a list had no recovery path. These are ordinary reversible list edits and should
not behave as destructive history boundaries.

## Decision

Trackknife records each completed add as one positional history entry and each
completed replacement as one snapshot-swap entry. Undo and redo retain complete
logical rows, metadata, revisions, and technical information under the existing
100-entry/64 MiB history bound. Initial workspace restoration remains a baseline
and creates no edit.

Cross-tab copies use the destination's add entry. The latest cross-tab move is
held as a workspace-level two-list snapshot transaction: one Undo restores the
source and removes the destination occurrences, and one Redo reapplies both.
The transaction is offered from either participating tab only while both lists
still match its expected state, so it cannot overwrite intervening edits.

## Consequences

Folder/file discovery and library Replace actions become reversible only after
their asynchronous result is applied. Cancellation before application creates no
entry. Large replacements may evict older history under the existing memory
bound.

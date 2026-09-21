# ADR-0211: Resume interrupted Up Next requests paused

Status: Accepted

## Decision

Extend ADR-0210's opt-in local paused resume to an interrupted Up Next request.
The existing version-1 request snapshot receives an optional, independently
versioned resume object on its first (interrupted) row. This binds position and
filesystem revision to the exact saved source, rather than a separate ordinal
checkpoint. The entire FIFO and offset are saved together by the persistence
worker, on request edits, the five-second checkpoint, and orderly shutdown.
Old snapshots still replay from the beginning; older clients ignore the additive
resume object. No database migration is required.

Restoration opens and seeks on the audio worker without activating output. It
requires the existing local resume preference and an unchanged file revision.
Failure remains visible and stops playback; remaining requests are not skipped.
Duplicate requests retain separate occurrences. Startup finishes request restore
before attempting ordinary list restore, and explicit playback actions win.

The saved return row is checked against list identity, raw path, source selection,
and segment. Restoring a detour does not consume the normal row again. A previously
consumed anchor can therefore return to its surviving continuation. Closed lists
remain closed; this feature does not revive an unrelated playback context.
As before, normal shuffle traversal is reconstructed rather than serialized.

Melody now permits its identity-checked checkpoint seek for the exact active
request occurrence. The existing server-owned FIFO, return candidates, paused
startup, and atomic checkpoint machinery remain authoritative. No new protocol
capability or client-owned server scheduler is needed. Stock MPD is unchanged.

## Validation

Local window tests cover duplicate requests, consumed anchors, opt-out, changed
files, the full 500-entry pending FIFO, saved offset, and no output activation. Existing ordinary
list resume and Up Next tests remain regression gates. Melody tests verify an
active request's paused offset, a pending duplicate, and eventual normal return.

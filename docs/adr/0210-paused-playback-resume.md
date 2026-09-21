# ADR-0210: Authority-owned paused playback resume

Status: Accepted

## Decision

Local playback offers an opt-in Settings → Playback preference, off by default,
to restore a list track and position paused. Loading and seeking run on the
audio worker without connecting PipeWire; explicit Play activates output.

The persistence worker writes a versioned checkpoint every five seconds and on
orderly close. It records list identity, row, revision-linked listening source
identity, and position. It stores no lossy presentation path. Restoration
requires the persisted row to match that source identity, followed by filesystem
revision validation on the audio worker. A stale ordinal must never seek another
track. Explicit playback actions invalidate pending asynchronous restoration.
Tab creation cannot overwrite the checkpoint before restoration reads it.

Melody retains its existing independent paused server-restart resume. Its
checkpoint now binds to queue occurrence ID, song ID, context, and track hash;
it follows occurrence reorders and rejects removed/mismatched tracks. Writes are
serialized, synced, and atomically replaced. Startup loading suppresses periodic
saves so they cannot replace the saved offset with a temporary zero position.
Legacy position-only checkpoints retain playback modes but do not restore an
unverified cursor or offset. Trackbench reconnect never changes server playback.
Stock MPD has no client-emulated restart restoration.

## Scope and limitations

Local resume requires an open, persisted list occurrence. Closed-list playback
and interrupted Up Next request positions remain unsupported. Melody also does
not restore the offset of an active Up Next request. This does not complete the
broader history/resume/album-shuffle priority. Checkpoint and list writes are
separate; after a crash, identity mismatches are rejected instead of guessed.

## Verification

Real-file audio tests cover paused loading, segmented positions, unchanged
position without output, stale revisions, and invalid/end positions. The window
test covers shutdown/startup restoration of a duplicate row, disabled restoration,
and no output activation. Melody tests cover reordered and removed occurrences,
legacy/mismatched identity rejection, startup overwrite protection, and concurrent
atomic checkpoint writes.

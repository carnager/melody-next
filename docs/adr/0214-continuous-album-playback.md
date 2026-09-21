# ADR-0214: Continuous album-oriented playback

Status: Accepted

## Decision

Add **Album shuffle** to Playback, the authority-specific status controls, and
the command palette. This is a traversal mode, separate from Edit → Shuffle
albums: it never rearranges the visible or stored list. Track Random and Album
shuffle are mutually exclusive. Preferences belong to each playback authority,
not the selected tab; activating any of its lists uses the chosen mode.

Use the ADR-0212 grouping key: exact album artist (artist fallback), album, date;
unknown albums are singleton occurrences. Preserve list order within each album,
including duplicates and non-contiguous occurrences. Enabling mid-album finishes
that album from the current occurrence. Previous can revisit earlier rows in the
current traversal. Every other album appears once in the cycle. Repeat draws a
fresh complete cycle, avoiding the just-finished album first when alternatives
exist. Single and Consume retain their existing semantics. Structural edits
start a fresh traversal anchored to the current occurrence; local metadata
changes also invalidate preparation. Melody rereads grouping metadata when
planning a new cycle.
Up Next suspends and resumes normal traversal instead of participating in album
grouping. Mode changes never restart or seek the current track.

Local grouping snapshots cached keys in 128-row event-loop batches, then builds
the traversal on the bounded Qt worker pool. Generation checks reject stale
results; end-of-track advancement waits for preparation rather than stopping
prematurely. Limits are one million rows, 64 KiB per key, 64 MiB total key text.
The Qt-free PlaybackOrder owns cycle selection, pending Repeat candidates, and
Previous/Next. Automatic local ReplayGain uses album gain in Album shuffle.

Melody advertises `melody_album_random 0|1` and reports `X-AlbumRandom` in status.
It owns the cycle and preloading independently of clients. Its standard random
flag remains on during album traversal, preserving request-queue integration;
an explicit stock `random` command exits album mode. Playback-state persistence
retains the mode. Cached metadata reads use the existing 10,000-entry/three-second
bound. A failed enable leaves the old mode and traversal intact. No stock MPD
client-side scheduler is introduced.

## Verification

Seeded core tests cover album contiguity, duplicate row occurrences, mid-album
activation, Previous, stable pending choices, and complete Repeat cycles. UI
tests exercise asynchronous grouping without list mutation and capability
visibility. Server tests exercise named-list playback, current offset/pause,
Up Next continuation, Repeat, status polling, and ordinary Random interoperability.

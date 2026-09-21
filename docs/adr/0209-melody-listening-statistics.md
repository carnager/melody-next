# ADR-0209: Server-owned Melody listening statistics

Status: Accepted — 2026-09-21

## Decision

Melody owns counting for its playback, independent of clients and Last.fm.
Trackbench never submits plays for MPD or endpoint audio and never combines
server statistics with its local history. Stock MPD remains unchanged.

Servers advertising `melody_stats` expose typed `X-PlayCount` and
`X-LastPlayed` (Unix milliseconds, zero means never) in song listings. The
existing optional Play count / Last played columns become available on Melody
queue/list tabs. Counts are right aligned; dates use the local display locale.
Absent values show a dash, not invented zero history. No new formatting or query
language fields are introduced.

The idle reader consumes generic response pairs so Melody's `stats` event is
not discarded by libmpdclient's fixed enum. Supporting sessions refresh listing
data despite an unchanged queue version, including a displaced queue; open
server playlists refresh through their coalesced existing worker path. Reads,
projection, and network activity remain off the GUI thread. Unknown idle
subsystems remain ignored and stock subsystem projection is preserved.

## Server contract and limitations

The implementation and normative protocol are in `../melody`, branch
`feature/listening-statistics`. A single worker observes primary-output position
and credits min(half the duration, four minutes), only for durations over 30s.
Pauses, large seeks, stalls, and long polling gaps do not earn time. Explicit
replays have distinct generations; output interruptions retain the occurrence.
Atomic SQLite occurrence insertion and aggregate update make retries idempotent.
SQL writes and provider network calls never hold playback/queue locks.

Melody reuses its documented metadata-derived rating hash. It survives moves
with unchanged tags, but identical metadata shares history and identity-field
retagging detaches it. It is not an audio fingerprint or the revision-linked
local identity from ADR-0207. There is no implicit cross-authority synchronization.

Restart retains aggregates but loses partial progress and starts a fresh
occurrence; restored position earns no retroactive credit. Write failures log
and retry while the occurrence remains current, without an unbounded queue;
an occurrence can be lost if it changes before persistence succeeds. No tags
are written. Server migration is additive/transactional with an empty-only
reverse script; old daemons can leave its tables intact without data loss.

History filters/sorting, skip counts, local resume/album shuffle, statistics
export/import, and server identity reconciliation remain separate follow-ups.

## Validation

Tests cover durable reopen, duplicate-event idempotence/conflicts, monotonic
timestamps, chunked reads, rescan/path identity, no-Last.fm collection, replay,
seek/pause/gap/short-track exclusion, protocol discovery, typed strict projection,
unknown versus zero display, right alignment, capability gating, and socket-level
stats invalidation with an unchanged queue version. Live audio/output handoffs
remain a manual deployment check; no running daemon is restarted by this change.

The development build and all 69 CTest suites pass, including the capability
gating/reconnect UI regression. Melody passes `go test ./...` and
`go test -race ./melodyd ./internal/lastfm`, including output reconnection and
empty/populated migration reversal checks. Formatting, SPDX, and diff checks pass.

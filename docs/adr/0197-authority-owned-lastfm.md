# ADR-0197: Authority-owned Last.fm scrobbling and loved tracks

## Status

Accepted and implemented, bounded, 2026-09-20 (post-M10 work).

## Decision

Melody accounts for its primary output once, regardless of connected clients or
additional synchronized outputs. Trackbench accounts only for its local audio
player, including when an MPD tab is selected. Its Melody endpoint never
scrobbles. Stock MPD has no client-side scrobbling fallback.

Both use Last.fm browser token/session authentication, with user-supplied API
key and shared secret. No account password is collected. Local and server
accounts are independent. A dedicated Settings page makes the authority clear;
authorization/disconnection and enable changes take effect immediately.
Melody credentials are sent only on explicit server setup through the existing
trusted MPD connection (which is not encrypted); use a trusted network/tunnel.
Credentials are never returned through status or logged. Each authority stores
a versioned private file (0600), using atomic replacement. No new dependency
or database migration is required. Removing that file resets the integration.

Listen accounting uses monotonic samples while playback position advances,
with each credited interval bounded by elapsed wall time. Pauses, buffering,
seeks, and long observation gaps add no listening time. Eligible tracks need
artist/title and duration greater than 30 seconds; submit once per observed
play after half the duration or 240 seconds, whichever comes first. Repeats and
request duplicates start separate plays. On restart partial listens are lost,
never inferred from a restored seek position. Now Playing is best effort.

Qualified scrobbles enter an ordered persistent outbox, bounded to 1000 entries.
Retry transport/service failures with backoff; invalid sessions pause delivery
until reauthorization. Permanent failures and provider-ignored submissions are
reported and removed. Ambiguous network failure can produce an at-least-once
retry; Last.fm offers no idempotency key. Account disconnect explicitly clears
pending submissions; disable stops collection/delivery without deleting them.
An account must be disconnected before changing its credentials.

Love/Unlove is an explicit artist/title action, independent of scrobbling enable.
Read loved state from Last.fm; do not conflate it with local ratings. Mutations
are not automatically retried. Loved dynamic playlists see changes on their
next explicit refresh under the existing exact library matching policy.

## References

- https://www.last.fm/api/desktopauth
- https://www.last.fm/api/scrobbling
- https://www.last.fm/api/show/track.love
- https://www.last.fm/api/show/track.getInfo

## Validation

The local accumulator is Qt-free `core::ListenAccounting`; Melody uses the same
accounting contract in `internal/lastfm`. Identity includes an audio playback
instance locally and queue occurrence plus explicit/natural restart generation
on Melody. Sparse position reports accumulate a bounded ten-second wall-time
window, while long polling gaps, backward jumps, and implausible forward jumps
receive no credit. Polling is conservative at transitions; sub-sample listening
can be lost. Account replacement clears partial listening; a different username
also clears pending entries and disables collection until explicitly enabled.

Validation includes the dedicated Qt service tests with a local fake HTTP server,
MPD protocol quoting/injection fixtures, UI authority/action checks, real local
audio regressions, Go account/outbox tests, and daemon primary-output ownership
tests. All six targeted CTest suites pass (lastfm-service, mpd-client,
mpd-session, local-audition, mpris-service, bench-main-window). Melody's full Go
suite and race-enabled service/daemon suites pass. Both development binaries
built; formatting, SPDX, and whitespace checks pass. Real
Last.fm credentials were not used: browser approval, provider-side propagation,
and every live output backend remain manual validation. No new dependency or
SQL migration was introduced.

Loved-state lookup transport correction: `track.getInfo` uses an unsigned HTTP
GET with API key and username, without a session key. Love/Unlove and scrobbling
retain signed POSTs. Both HTTP fixtures assert this distinction and preserve
artist/title encoding. References: [track.getInfo](https://www.last.fm/api/show/track.getInfo)
and [REST requests](https://www.last.fm/api/rest). The Qt Last.fm service and Go
Last.fm/daemon suites pass; the reported live HTTP 500 has not been reproduced
with provider credentials.

Setup refinement: retain user-owned application credentials (no bundled key or
secret). Guide registration in Settings, reuse the entered key for dynamic
playlists on explicit Connect with an opt-out checkbox, and keep the Metadata
services field synchronized so Settings Save cannot restore an older value.
Only the API key is copied into local read-service settings, never the secret.
Each authority accepts `begin` without arguments to reconnect using its own
saved credentials; status exposes only credential-presence and pending-auth
booleans. The setup fields hide after credentials are saved. Service fixtures
cover signed reconnects and credential removal; UI tests cover input validation,
key reuse, and opt-out. Live browser approval remains manually validated.

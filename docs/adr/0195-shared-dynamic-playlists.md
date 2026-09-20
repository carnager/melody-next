# ADR-0195: Shared dynamic playlists and desktop refinements

Date: 2026-09-20

Status: accepted

## Decision

**Trackknife decision:** dynamic playlists use one definition, editor, and
execution service for both library authorities. A typed query adapter evaluates
`tkq-1` against the local SQLite index or translates it to the connected
MPD/Melody server’s advertised filter grammar. Stock MPD supports compatible
tag rules; rating and technical-field rules require Melody extensions. Results are a variant of a complete
local-row vector or a complete MPD-track vector, never mixed rows. Local raw
paths survive unchanged. Unsupported server expressions fail explicitly.

File → Dynamic playlists opens a modeless editor for the active authority.
Definitions are named and persist independently of ordinary playlists. Each
catalog belongs either to the local library or one MPD connection profile.
Settings stores an explicit version-1 JSON envelope; each definition retains
exact query source, dialect, dialect version, and compiler schema. Unknown
versions are reported and not overwritten by the editor. This adds no SQLite
migration and follows the workspace's existing settings backup contract.

Rules can use tags, ratings, and the existing query language, with a maximum
result count (1–500) and optional shuffle. Ratings retain their 0–10 meaning.
While the editor is open, an evaluated rule refreshes on library invalidation
and every 30 seconds to pick up externally changed ratings. Polling does not
reshuffle a shuffled list; Refresh and library invalidation do. Stop cancels
work and automatic refresh until another explicit Refresh. Server queries
retain the existing 20,000-track search bound and fail visibly at that boundary
rather than silently sampling an incomplete response. Local evaluation uses cached index
metadata and never triggers a scan or reads media files.

The shared Last.fm provider supports similar tracks from a seed artist/title,
a user's loved tracks or all-time top tracks, and top tracks for a tag.
Requests are explicit Refresh actions, over HTTPS with a timeout and response
size cap; no background provider polling or scrobbling. The user supplies an
API key in Metadata services, which links to registration. Definitions never
contain credentials. Provider responses are bounded to 500 candidates and
matched serially through the same library adapter. Exact normalized
artist/title matches only; duplicate URIs/paths are removed, ambiguous editions
use deterministic path order, and unmatched recommendations are counted.
MusicBrainz IDs are retained from the response but are not yet used to resolve
alternate credits/editions. Last.fm never supplies stream URLs or permission to
play music outside the selected library.

The dynamic result window is read-only and uses the shared track-view engine.
Open snapshot creates an ordinary working tab; Add to queue is explicit in
server context. Evaluating or refreshing never rewrites a playing queue or a
stored server playlist. Changing authority closes the editor; changing or
losing the server connection invalidates its results. Cancellation generations
reject late provider, index, and server results.

**Proposal:** queue replenishment/radio, automatically synchronized stored server
playlists, a visual rule builder, and MusicBrainz-aware provider matching are
separate follow-ups. They are not implicit behavior of this first implementation.

## Small desktop refinements

The library ordering control is a labeled dropdown. Advertised
`melody_albums_latest` supplies the complete cached album-artist ranking for
Melody's default tree; other tags and stock MPD retain the bounded sorted-search
fallback. Client caching is scoped to tag and connection, invalidated by
library changes and reconnect. No Melody protocol extension is necessary.

Notification enablement now means track changes can notify while the window is
focused too. A separate background-only checkbox restores the former policy.
Notifications remain off by default, transient, normal urgency, and silent
(`suppress-sound`). Settings offers an explicit test and displays desktop
acceptance or delivery failure; desktop acceptance does not claim a visible
popup when Do Not Disturb or desktop rules suppress it. Runtime errors appear
in the status bar. This supersedes ADR-0144's hard-coded background-only and
silent-failure rules.

Metadata services links to AcoustID application registration, not the user-key
page: lookup uses an application/client key.

## Provider references

- [AcoustID web service and application registration](https://acoustid.org/webservice)
- [Last.fm similar tracks](https://www.last.fm/api/show/track.getSimilar)
- [Last.fm user top tracks](https://www.last.fm/api/show/user.getTopTracks)
- [Last.fm loved tracks](https://www.last.fm/api/show/user.getLovedTracks)
- [Last.fm tag tracks](https://www.last.fm/api/show/tag.getTopTracks)
- Melody `melodyd/db.go::rofiAlbumsResponse` and `cmdMelodyAlbumsLatest` in
  the sibling repository establish the cached response's field order.

## Validation

The development build passes. The final workspace suite passes all 116 test
cases plus setup/cleanup, and the local-library suite passes, including a
real-FLAC regression proving rule invalidation, cached offline results, and
non-UTF-8 raw-path preservation. Shared-service fixtures cover both result
authorities, query compilation, late-result cancellation, duplicate/missing
recommendations, malformed/provider-error responses, catalog isolation, and
unsupported catalog versions. The editor test covers saving, reopening, and
empty-result actions. Existing settings tests cover the dropdown’s authority
visibility and persistence, masked key entry, and registration-link presence.

MPD client/session/model/library-tree and MusicBrainz regression suites pass.
An isolated `dbus-run-session` notification fixture verifies asynchronous
acceptance, replacement IDs, normal urgency, silent hints, and delivery errors
without posting to the user's desktop. Formatting, diff whitespace, and SPDX
checks pass. Settings was inspected at 720×480 and the dynamic editor at
900×720. Last.fm parsing/matching is fixture-tested; no live API-key request
or desktop popup visibility is claimed.

### Snapshot presentation

Dynamic results and opened snapshots use plain columns: one line per track,
without album headers or an artwork gutter. Local snapshots persist this in
the existing per-list view layout. MPD snapshots retain a profile-scoped name
marker so reopening restores the same flat presentation; the marker follows
confirmed renames and is removed on confirmed deletion. Ordinary playlist
defaults remain independently configurable.

### Last.fm refresh variety

Each explicit provider refresh requests up to 500 candidates independently of
the requested playlist length. The shared engine matches the whole bounded
pool, then randomly selects up to the playlist limit, preferring matches that
were absent from the previous successful refresh. Only when that unseen pool
is exhausted are previous tracks reused. This minimizes overlap without
producing an unnecessarily short playlist. When every available match fits,
all are included and the UI says so; new membership cannot be guaranteed for
an undersized provider/library pool.

Selection varies even with Shuffle unchecked. That checkbox controls final
ordering: unchecked retains provider rank within the fresh subset; checked
randomizes order as well. Rule-based playlist behavior is unchanged.

The previous selection is stored as SHA-256 identity digests, scoped to library
authority/profile, definition ID, provider source, and seed/user/tag. It
survives closing the editor and restarting. Credentials and raw local paths
are not stored in this history. Only successful nonempty completions update
it; cancelled, failed, and empty refreshes leave it intact.

Regression cases exercise both local and MPD result types, disjoint successive
selections when enough matches exist, persistence across service instances,
minimum overlap with small pools, order policy, and cancellation/empty-result
history preservation.

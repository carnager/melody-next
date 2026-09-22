# ADR-0216: History ordering and definition-owned result interactions

Status: Accepted

## Decision

Add `SORT [ASCENDING|DESCENDING] HISTORY(name)` to tkq-1. This was invalid
syntax previously; existing `SORT BY` tkfmt expressions must not be
reinterpreted as statistics. Numeric history keys use stable ordering, with
missing timestamps before known values ascending (after them descending).
Counts default to zero. All six ADR-0215 statistics are supported.

Current-tab searches retain source occurrences, including duplicates, and use
the owning authority's history, never rendered column text or unloaded caches.
Server evaluation and history aggregation belong to Melody. Advertise and gate
new history ordering support independently of history filtering.

Dynamic results remain owned by their definition. Expose applicable ordinary
track actions, outward drag, playback, ratings, and Up Next. Manual membership
and ordering edits require an explicitly editable snapshot; refresh never
overwrites that snapshot or a playing queue. Preserve selection/current row
and scroll by source identity across result refresh, distinguishing duplicate
occurrences. Playback markers reflect the player's source identity, not a row
number in a refreshed result. Keep local and server authority separate.

Workers remain bounded/cancellable and stale responses must be discarded.
No database migration or file metadata mutation is part of this change.

## Scope and bounds

Ordering applies to query results, not a mutation of the source list. Local tab
history aggregates the whole indexed album plus distinct additional qualified
tab sources (including logical tracks); duplicate occurrences contribute once.
It uses one read transaction and never probes the filesystem. Unknown revisions
fail explicitly. The local tab bound is 100,000 sources; library/history snapshots
remain bounded to one million records. Melody list search preserves server-owned
occurrences and is separately advertised; named/unnamed list bounds are 10,000 /
20,000 entries with no silent truncation. Older servers fail with an explanation.

The dynamic editor remains the live result surface. Persistent autoplaylist tabs,
definition exclusions, calendar-relative history operators, and grouped search
presets are not implemented by this ADR. Search presets grouped by exploration,
ratings, listening, audio properties, and metadata completeness are a user-raised
proposal for the next discovery layer, not yet a capability claim.

## Validation

The 69-suite development CTest run passes. Focused coverage includes numeric
ordering (2 versus 10), missing timestamps, explicit syntax/capability refusals,
library-wide album history from a partial tab, duplicate and logical sources,
raw paths, cancellation, selection/scroll restoration in both result models,
source-qualified Up Next/copy/tools, and duplicate local playback markers.
Melody passes `go test ./...`, `go test -race ./melodyd ./internal/lastfm`, and
`go vet ./melodyd`; protocol tests cover named/stashed lists, duplicate entries,
whole-library album statistics, sort order and non-mutation. Both binaries build.
No production deployment or interactive desktop acceptance is claimed here.

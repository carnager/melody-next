# ADR-0206: Refresh dynamic rules after local library changes

## Status

Accepted, 2026-09-21. First slice toward persistent autoplaylist tabs.

## Decision

The local library publishes a content-change notification after an explicit
scan finishes, including partial/cancelled scans that retain committed updates,
and after its coalesced refresh for committed metadata/path/root changes.
Open dynamic rule editors reevaluate on that notification, as they already do
for rating changes and remote database events. Queries remain index-only.

If the library changes during evaluation, mark the result stale and coalesce
the changes into a single follow-up evaluation. Do not publish the obsolete
result over the previous display. Stop, editing the definition, or losing the
authority cancels the pending refresh. Provider sources remain explicit and
never make Last.fm requests because the local index changed.

Saved Search result tabs and opened dynamic snapshots retain their existing
snapshot semantics. Persistent live tabs, membership ownership, occurrence
reconciliation during playback, and custom grouping remain subsequent work.

## Verification

Regression tests cover coalesced changes during an outstanding query, stale
result suppression, fresh result publication, and stopping a pending refresh.

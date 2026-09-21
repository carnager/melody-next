# ADR-0208: Local listening-history columns

## Status

Accepted, 2026-09-21.

## Decision

**Trackknife decision:** local working lists expose optional **Play count** and
**Last played** columns through the existing header **Columns** menu and
Workspace track-layout controls. Both are hidden by default. Counts align
right; dates use the desktop locale and local time, with an ISO timestamp in
the tooltip. Last played is qualification time, as defined in ADR-0207.

An unplayed, revision-qualified source shows `0` and `Never`. Loading shows an
ellipsis; a missing source revision or failed lookup shows a dash with an
explanation, never a false zero. Re-enabling a history column retries failed
lookups. Opening a layout never scans files or creates history identities.

Columns use stable semantic IDs `play-count` and `last-played` in the existing
version-1 layout. The established additive-column migration appends them hidden
to older layouts while preserving order, widths, and visibility. Copying a
local layout to MPD cannot expose local statistics: those physical columns
remain hidden, and their actions are absent in MPD context. Dynamic-playlist
previews and Up Next do not expose history in this slice.

Layout capture preserves a hidden column's preferred width instead of Qt's
reported zero width, so enabling it does not collapse its label and contents.

History remains repository data, not metadata or row-owned persistent state.
These columns introduce no `tkfmt-1` or `tkq-1` fields and cannot shadow a tag
in an existing saved expression. Querying or sorting by statistics is deferred.

## Loading and invalidation

Model display requests coalesce into batches of at most 64 qualified sources
on the existing persistence worker. Each model has one read in flight, at most
64 waiting rows, and a 512-entry display cache. The service admits at most four
pending reads across models. It performs read-only indexed lookups using the
cached source revision, without filesystem probes or identity insertion.

Successful listening writes invalidate display caches. Source edits and list
structural changes discard stale cache entries; generation checks reject late
worker results, and destruction-safe callbacks cannot update closed tabs.
Only displayed/requested rows reload. History updates neither reset the model
nor create list edits. They do not invalidate album grouping or recompute
selection summaries. Selecting a column is not permission to scan a collection.

## Verification

Repository coverage proves absent-source lookups are read-only and stored
statistics resolve by source. Workspace tests cover asynchronous loading,
duplicate/logical rows, unknown revisions, live updates without selection/reset
changes, locale formatting, right alignment, layout round trips, MPD exclusion,
bounded admission, stale replies after replacement, and errors distinct from
unplayed sources. Shared layout tests cover hidden additive-column migration.

Validation (2026-09-21): full development build and all 69 CTest suites pass;
the complete workspace suite also passes after the hidden-width correction.
An offscreen screenshot was inspected for readable headings, right-aligned
counts, and dates. Changed-file formatting, SPDX, and diff checks pass.

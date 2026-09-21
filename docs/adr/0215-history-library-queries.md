# ADR-0215: Explicit listening-history query operands

Status: Accepted

## Decision

Add `HISTORY(name)` as an explicit tkq operand in Library and Server searches,
saved queries, and dynamic rules. It accepts `playcount`, `lastplayed`,
`dayssinceplayed`, `albumplaycount`, `albumlastplayed`, and `albumdayssinceplayed`.
Only numeric and PRESENT/MISSING comparisons are accepted. Boolean composition
uses existing grammar. History sorting and current-tab history queries are not
included; the latter fail visibly rather than treating unloaded history as zero.

This is an additive tkq-1 grammar extension: the accessor syntax was previously
invalid. No previously valid source changes meaning. Bare `playcount`, quoted
fields, and `HISTORY IS value` still refer to file tags. No formatting-language
semantics change. Existing persisted source/dialect records remain authoritative.
The dedicated history corpus records dialect, context, source, result, rationale.

Counts default to zero. Last-played values are UTC Unix milliseconds; no play
means no timestamp. Age is complete 24-hour days at evaluation time, clamped to
zero for future timestamps, and is missing for never-played tracks. A numeric
comparison never matches missing timestamps/ages, even with a negative operand.
Album count is the sum of track counts; album last played is the most recent
qualified listen to any indexed track on that album. Aggregate before applying
other predicates so queries never accidentally call a partially played album
unplayed. Use each authority's established album identity, with unnamed albums
treated as separate tracks. Local and server history are never merged.

Local evaluation runs on the existing cancellable library worker, reading a
consistent SQLite snapshot. It joins indexed raw-path/revision observations to
the existing durable source identities; it never stats files or substitutes
tag hashes for local listening identity. The index currently represents whole
files, so logical/subsong listens are not silently attributed to whole files.
Bound each source/statistic/index snapshot to one million records and retain the
existing 100,000-match limit. No schema migration or metadata writes are needed.

Melody advertises `melody_history_filters`; typed translation maps the accessor
to `history-*` predicates in its filter tree, including searchalbums. An older
daemon rejects capability-gated translation instead of returning false matches.
Server aggregates cover its full library before narrowing results. Open dynamic
rule previews invalidate when the appropriate authority records a listen.

## Examples

```
HISTORY(albumplaycount) EQUAL 0
HISTORY(playcount) GREATER 4
HISTORY(albumplaycount) EQUAL 0 OR HISTORY(albumdayssinceplayed) GREATER 180
```

The last example means never played or over 180 complete days old—not six
calendar months. Exact calendar-relative operators remain separate work.

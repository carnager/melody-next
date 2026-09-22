# tkq-1: the library query language

Decided by [ADR-0150](adr/0150-tkq-query-dialect.md). This document is
the normative specification for dialect `tkq`, version 1. The surface
is foobar2000-inspired, but no external compatibility is promised and
foreign queries are not expected to run — the same stance ADR-0008
took for `tkfmt-1`. Queries and formatting remain different languages
with separate parsers; a query may embed `tkfmt-1` where noted.

Persisted queries always store the exact original source together with
`dialect`, `dialect_version`, and the compiler schema
(see [compatibility](compatibility.md)); a compiled AST alone is never
persisted.

## Evaluation scope

Library scope evaluates the cached local index without filesystem scans
(ADR-0116); rows written before migration 30 carry field rows and technical
columns only after their next explicit Refresh. Current tab evaluates captured
occurrences; technical queries may probe missing technicals through the existing
worker, while history queries never require a filesystem probe. Server scopes
use capability-gated protocol translation and server-owned library/history.

## Grammar

For ready-made starting points, open **Workspace → Search → Browse presets**.
Explore, Favourites, Listening, Audio properties, and Library maintenance contain
25 presets. Choose a preset, enter its value if requested, then inspect or edit
the generated query in the input field. **Save as…** keeps your adjusted query
separately from the built-ins. The selected scope is preserved; unsupported
server presets are hidden. This is not a general visual query builder.

```text
query       = "ALL" | simple-words | expression [ sort-clause ]
expression  = and-expr { "OR" and-expr }
and-expr    = unary { "AND" unary }
unary       = "NOT" unary | "(" expression ")" | predicate
predicate   = lhs "HAS" string
            | lhs "IS" string
            | "*" "HAS" string
            | lhs ("GREATER" | "LESS" | "EQUAL") integer
            | lhs ("PRESENT" | "MISSING")
sort-clause = "SORT" [ "ASCENDING" | "DESCENDING" ]
              ( "BY" tkfmt-source | "HISTORY" "(" statistic-name ")" )
lhs         = word | quoted-string
            | "HISTORY" "(" statistic-name ")"
string      = word | quoted-string
```

- Keywords are uppercase and reserved: `ALL AND OR NOT HAS IS GREATER
  LESS EQUAL PRESENT MISSING SORT ASCENDING DESCENDING BY`. Lowercase
  spellings are ordinary text.
- Precedence: `OR` binds loosest, then `AND`, then `NOT`; parentheses
  group explicitly.
- Strings quote with double quotes; a doubled quote (`""`) is a
  literal quote. Multi-word comparison values must be quoted.
- An `lhs` containing `%`, `$`, or `#` is a `tkfmt-1` expression
  predicate (quote it when it contains spaces): the expression
  evaluates per row and its text feeds the operator. Bare truthiness
  is not a predicate — an expression always pairs with an operator.
- `sort-clause`: everything after `BY` to the end of the input is
  `tkfmt-1` source, compiled in the sort context. One trailing clause;
  default direction is ascending.

### Simple words

If the token stream contains no reserved keyword, parenthesis, quote,
or `*`, the query is an all-word search equivalent to
`* HAS <words>` — typing `miles blue` just works. Any structured
token makes parsing strict: a malformed query is a positioned error
and never silently degrades into a word search.

`ALL` matches every indexed track and stands alone (an optional sort
clause may follow).

## Semantics

Comparison text normalizes with simple Unicode lowercasing — the same
normalization the index stores beside the original spelling, which is
never destroyed. There is no diacritic folding in v1.

Field names are index-canonical (lowercased, separators stripped), so
`replaygain_track_gain`, `REPLAYGAIN_TRACK_GAIN`, and
`ReplayGain Track Gain` name the same field.

For multi-value fields:

- `IS`: some single value equals the string (casefolded).
- `HAS`: every word of the string occurs in at least one value; each
  word independently.
- `PRESENT` / `MISSING`: the field carries at least one value / none.
- `GREATER` / `LESS` / `EQUAL`: the leading integer of some value
  satisfies the comparison; values without a leading integer never
  match.
- `* HAS`: every word occurs somewhere in the row's indexed text (any
  field value or the denormalized search text).

`date GREATER|LESS|EQUAL n` compares the year prefix of the indexed
date value.

Technical pseudo-fields resolve to typed index columns retained from
the scan's probe: `codec` (text operators), `samplerate`,
`bitspersample`, `channels`, `length_ms` (numeric operators;
`PRESENT`/`MISSING` test whether the probe knew the value).

`rating` and `albumrating` (ADR-0179) resolve to the stored 0-10
content-identity ratings — `rating GREATER 7`, `albumrating EQUAL 10`,
`rating PRESENT`/`MISSING` for rated/unrated. Both are also available
to embedded expressions as `$info(rating)`/`$info(albumrating)`. Like
the technical pseudo-fields, they shadow same-named file tags; a
`RATING` file tag is not reachable by name from a query.

`tkfmt-1` expression predicates evaluate against the row's indexed
fields and technicals; the resulting text is lowercased and compared
per the operator (`MISSING` means the expression produced empty text).

## Server library scope

### Listening history (ADR-0215)

Library, Server, and Current tab scope support explicit `HISTORY(...)` operands. Enable
**Query** in Search. These also work in saved searches and dynamic playlist
rules. Ordinary bare fields still refer to file metadata, not listening history.

- `HISTORY(playcount)` counts qualified listens; unplayed tracks have zero.
- `HISTORY(lastplayed)` is a UTC Unix timestamp in milliseconds, or missing.
- `HISTORY(dayssinceplayed)` is age in complete 24-hour days, or missing.
- `HISTORY(albumplaycount)` sums track counts across the indexed album.
- `HISTORY(albumlastplayed)` and `HISTORY(albumdayssinceplayed)` use the most
  recent qualified listen to any track on that album.

Use GREATER, LESS, EQUAL, PRESENT, or MISSING; HAS and IS are rejected. Missing
timestamps/ages never satisfy numeric comparisons. Album statistics cover the
whole indexed album before other predicates are applied. Unnamed albums are
single-track groups. Local and server statistics are independent.

```
HISTORY(albumplaycount) EQUAL 0
HISTORY(playcount) GREATER 4
HISTORY(albumplaycount) EQUAL 0 OR HISTORY(albumdayssinceplayed) GREATER 180
```

The last example includes never-played albums and albums last played over 180
complete days ago; it does not mean six calendar months. Local queries use
revision-qualified whole-file index identities, without rescanning files;
logical/subsong listens are not attributed to their containing whole file.
Current-tab searches preserve duplicate occurrences. Local queries read a
consistent history snapshot qualified by the tab's source revisions, including
logical tracks and files outside the index, without filesystem probes. Album
aggregates include the whole indexed album and any additional distinct sources
in the tab; duplicate occurrences never inflate the aggregate. Unqualified local
rows report that a source revision is required rather than pretending to be
unplayed. Melody must advertise `melody_history_filters`; Current tab additionally
requires `melody_list_search`, which evaluates the named list or stashed unnamed
queue on the server. Stock MPD/older Melody report unsupported scope explicitly.

### History ordering (ADR-0216)

Use `SORT HISTORY(name)` for ascending numeric order, or
`SORT DESCENDING HISTORY(name)` for descending order. All six statistics above
are supported. Missing timestamps/ages come first ascending, last descending;
equal keys retain source order (the deterministic library order for Library
scope, occurrence order for Current tab).

```
ALL SORT DESCENDING HISTORY(playcount)
ALL SORT HISTORY(lastplayed)
HISTORY(playcount) EQUAL 0 SORT HISTORY(albumplaycount)
```

This orders search results, not the source list. Open results to obtain a
separate ordered list. Existing `SORT BY` remains tkfmt-1; `%playcount%` is still
a metadata tag, not a history accessor. Melody history ordering requires the
separately advertised `melody_history_sort` capability.

### Metadata translation

The Search dialog runs the same dialect against a connected Melody server
that advertises `searchalbums`: supported queries translate mechanically to
the server's filter grammar (word search, tag HAS/IS terms, numeric
comparisons on `rating`, `albumrating`, `samplerate`, `bitspersample`,
`channels`, and `length_ms` in whole seconds, `rating PRESENT`, and simple
single-`%field%` sorts). Results open as a committed MPD search tab;
MusicBrainz field names translate to the server's underscore spellings
automatically.

Against a server that also advertises `filtergrammar` (Melody grammar
level 2), the whole boolean surface translates: `OR` and `NOT` trees,
`PRESENT`/`MISSING` on ordinary tags (the MPD empty-value forms; rating
fields negate their numeric form), and `GREATER`/`LESS`/`EQUAL` on
ordinary tags including `date` by leading integer. Older servers join
terms with AND only, so those constructs report "not supported by this
server" there instead of running with changed meaning. tkfmt expression
predicates never run server-side, and `PRESENT`/`MISSING` on the
probe-derived technical pseudo-fields stays local-only.

## Bounds

Source ≤ 4096 bytes, ≤ 256 AST nodes, ≤ 64 words per string. Embedded
`tkfmt-1` programs run under the standard evaluation limits. Result
sets are capped at 100 000 matches, pages at 200 rows; evaluation is
cancellable throughout.

## Planner

Indexable predicates push down into SQL over the per-value field table
and the typed technical columns; only available files match.
`tkfmt-1` expression predicates evaluate per candidate row; pushable
`AND`-conjuncts still pre-filter the candidate stream, and an
`OR`/`NOT` subtree containing an expression predicate evaluates wholly
per row. A sort clause materializes the bounded match set, computes
per-row keys, and stable-sorts over the deterministic default order
(artist, album, disc, track, title, path) so equal keys keep a stable
tiebreak.

## Deferred beyond v1

Recorded so the spec stays honest: time operators
(`AFTER`/`BEFORE`/`SINCE`/`DURING`, `DURING LAST n <unit>`), calendar-relative
history comparisons; diacritic folding; regular
expressions; path-targeted operators; autoplaylists,
and the query builder UI (separate Area 2 packages).

Saved query/word definitions with explicit dialects and scope are implemented
in ADR-0163; see [Saved searches](local-library.md#saved-searches).

## Index completeness

Database queries require complete indexed field evidence (ADR-0166). Older
indexes that may have truncated tags report a Refresh requirement instead of
treating omitted values as missing. Refresh runs only on explicit request; rerun
the query afterwards. Field indexing now uses overall per-file bounds of 4096
field names, 16384 values, and 4 MiB of name/value text, with explicit failure
instead of silent truncation.

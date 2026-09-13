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

Queries evaluate against the cached local library index only. Query
evaluation never scans the filesystem (ADR-0116); rows written before
migration 30 carry field rows and technical columns only after their
next explicit Refresh.

## Grammar

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
sort-clause = "SORT" [ "ASCENDING" | "DESCENDING" ] "BY" tkfmt-source
lhs         = word | quoted-string
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

`tkfmt-1` expression predicates evaluate against the row's indexed
fields and technicals; the resulting text is lowercased and compared
per the operator (`MISSING` means the expression produced empty text).

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
(`AFTER`/`BEFORE`/`SINCE`/`DURING`, `DURING LAST n <unit>`) until
playback statistics exist in the index; diacritic folding; regular
expressions; path-targeted operators; autoplaylists,
and the query builder UI (separate Area 2 packages); MPD-side
translation.

Saved query/word definitions with explicit dialects and scope are implemented
in ADR-0163; see [Saved searches](local-library.md#saved-searches).

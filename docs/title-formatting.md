# Trackknife formatting expressions

## Contract

**Trackknife decision:** `tkfmt-1` is Trackknife's own versioned, deterministic
formatting-expression language. Its surface syntax is intentionally familiar to
users of MusicBrainz Picard and foobar2000, but neither language is a behavioral
compatibility target.

The language formats existing context data. It cannot modify metadata, files,
library state, playlists, the queue, settings, or the current time. Mutation is
performed by separate declarative operations with preview, conflict detection,
commit, and recovery.

## Initial hosts

One parser, compiler, and evaluator serves all hosts. A host controls which
fields exist and how the resulting text is consumed; it does not fork the
grammar.

| Host | Expression role | Host-only work after evaluation |
| --- | --- | --- |
| Library tree | One expression per configured hierarchy level | empty-label policy and branch presentation |
| Track/playlist view | Column, group label, summary, or stable sort key | painting, collation, sort direction |
| Queue view | Column, group label, or stable sort key | queue-position/state fields |
| Now playing | Status, notification, or window text | multiline and length policy |
| Conversion/file plan | Relative destination path | component validation, sanitization, traversal and conflict checks |
| Copy/export | Clipboard or report text | row/record separators and destination encoding |
| Metadata transformation | Derive one staged scalar value | a separate declarative chain assigns the result to its target field |

Library hierarchy is structural configuration. A preset stores an ordered list
of level expressions; a `/` appearing in an artist or album value never creates
an accidental tree level.

A conversion expression may render separators to request subdirectories, but
the result is still untrusted text. The file-operation planner owns path parsing,
raw OS-path conversion, sanitization, containment, collision handling, preview,
and commit.

## Evaluation model

Every scalar expression evaluates to one UTF-8 string. Conditions use ordinary
string truthiness:

- the empty string is false;
- every non-empty string, including `0`, is true;
- a missing field evaluates to the empty string;
- boolean and comparison functions return `1` for true and the empty string for
  false.

Concatenation joins child strings in source order. There is no separate truth
flag, no hidden missing-field marker, and no optional-section syntax.

Programs are immutable after compilation and safe for concurrent evaluation.
Evaluation state is local to one call. `tkfmt-1` has no assignment or variable
functions.

## Syntax

### Text

Text is emitted as written, including spaces, apostrophes, square brackets, and
physical newlines. Backslash escapes syntax characters:

```text
\$  \%  \,  \(  \)  \\
```

An unnecessary or trailing backslash is a compile error. Users can place UTF-8
characters directly in source; the source and result must be valid UTF-8 before
an operation is committed.

### Fields

`%artist%` resolves a context field case-insensitively. Field names may contain
spaces, hyphens, underscores, and colons. The host provides both metadata and
virtual fields such as queue position or playback state.

Ordinary field rendering joins ordered multi-values with `; `. Use the explicit
multi-value functions when a different behavior is needed.

Field resolution performs no implicit album/artist fallback or track-number
padding. Shipped presets express fallback and padding visibly:

```text
$if2(%albumartist%,%artist%)
$num(%tracknumber%,2)
```

### Functions

Functions use `$name(argument,...)`. Arguments are expressions, may be empty,
and may contain nested calls. A comma or parenthesis that should be text must be
escaped. Function names are ASCII case-insensitive; persisted examples use
lowercase.

```text
$if(%album%,%album%,Unknown album)
$replace(%title%\, live,live,concert)
```

Unknown functions, invalid arity, malformed syntax, and use outside an allowed
host are compile errors. Lazy functions evaluate only the branch they select.

The descriptive grammar is:

```ebnf
expression = { text | escape | field | call } ;
field      = "%" field-name "%" ;
call       = "$" function-name "(" [ argument { "," argument } ] ")" ;
argument   = expression ;
escape     = "\\" ("\\" | "$" | "%" | "," | "(" | ")") ;
```

The parser retains the original source and UTF-8 byte spans for editor
diagnostics. Editor recovery may produce a partial syntax tree, but executable
compilation fails closed.

## Metadata and context fields

Field names are normalized with ASCII case folding for lookup. Hosts backed
by the library index -- Trackknife's library views and queries, and the
engine's `playback.format` and `catalogue.find` -- go further: they look a
field up by its canonical name, which also ignores spaces, underscores and
hyphens, so `%replaygain_track_gain%` and `%Replay-Gain-Track-Gain%` name one
field there. Files keep their tags' native spellings; only that lookup folds
them, and two tags whose names differ only so read as one field with both
values, in order. Values retain
their original UTF-8 bytes; there is no implicit Unicode normalization.

The context must distinguish:

- an absent field;
- a present field with an empty value;
- an ordered field with multiple values;
- technical information, exposed explicitly through `$info`;
- host virtual fields such as `queue_position`.

Suggested common virtual fields use snake case to avoid collision with normal
tags:

| Host | Fields |
| --- | --- |
| Track/list | `list_index`, `list_total`, `selected` |
| Queue | `queue_position`, `queue_total`, `queue_origin` |
| Now playing | `playback_time`, `playback_remaining`, `playback_state`; with the engine's library row also `length`, `path`, `rating` (1-10) |
| Conversion | `conversion_index`, `source_filename`, `source_extension` |
| Metadata transformation | none; fields are the current working document after earlier actions |

The typed host defines availability. A missing host field behaves like any other
missing field; presets should use `$if2` for fallback.

## Built-ins for `tkfmt-1`

Only functions in this section belong to the dialect. Adding one requires tests
and a documentation update; changing existing behavior requires a new dialect
version.

### Conditional and comparison

| Function | Result |
| --- | --- |
| `$if(condition,then[,else])` | Lazily evaluates one branch. Missing `else` yields empty. |
| `$if2(value,...)` | Returns the first non-empty argument, evaluated left to right. |
| `$and(value,...)` | `1` when every argument is non-empty; empty argument list is true. |
| `$or(value,...)` | `1` when at least one argument is non-empty; empty argument list is false. |
| `$not(value)` | `1` when `value` is empty. |
| `$eq(a,b)` / `$ne(a,b)` | Byte-exact UTF-8 equality/inequality. |
| `$eqi(a,b)` | Unicode simple-case equality without normalization or full folding. |
| `$gt(a,b)`, `$gte(a,b)`, `$lt(a,b)`, `$lte(a,b)` | Signed integer comparison. |

`$and` and `$or` short-circuit. This matters for limits and errors even though
the language has no side effects.

### Integer

| Function | Result |
| --- | --- |
| `$add(a,...)` | Sum; empty argument list returns `0`. |
| `$sub(a,b,...)` | Left-associated subtraction; requires at least one argument. |
| `$mul(a,...)` | Product; empty argument list returns `1`. |
| `$div(a,b,...)` | Signed integer division truncated toward zero. |
| `$mod(a,b)` | Signed remainder with the dividend's sign. |
| `$min(a,...)` / `$max(a,...)` | Minimum/maximum; require at least one argument. |
| `$num(value,width)` | Decimal value padded with leading zeroes; the sign counts toward width. |

Integer conversion trims ASCII whitespace, accepts one optional `+` or `-`, and
then requires at least one decimal digit and no other characters. Invalid input
converts to zero. Overflow and division by zero are evaluation errors.

### Text

Character indexes and lengths count Unicode scalar values, not UTF-8 bytes,
grapheme clusters, or display cells.

| Function | Result |
| --- | --- |
| `$lower(text)` / `$upper(text)` | Locale-independent simple Unicode case mapping. |
| `$trim(text)` | Removes leading and trailing ASCII whitespace. |
| `$len(text)` | Unicode scalar-value count. |
| `$left(text,count)` / `$right(text,count)` | At most `count` characters; negative count is an error. |
| `$longest(value,...)` | First longest argument; requires at least one. |
| `$repeat(text,count)` | Repeats text; negative count is an error. |
| `$replace(text,search,replacement[,search,replacement...])` | Applies pairs from left to right; empty search is an error. |
| `$pad(text,width[,fill])` | Right-pads without truncation; `fill` must be exactly one Unicode scalar and defaults to a space. |

All output-producing functions enforce the evaluation byte limit before
allocating their result.

### Multi-value and technical data

These functions take a non-empty literal field name as text, not a `%field%`
value. This keeps cache dependencies complete at compile time:

| Function | Result |
| --- | --- |
| `$get(name)` | The field's values joined with `; `, or empty when absent. |
| `$getmulti(name,index)` | Zero-based ordered value, or empty when absent/out of range. |
| `$join(name,separator)` | All ordered values joined with `separator`. |
| `$lenmulti(name)` | Number of ordered values; absent is `0`. |
| `$info(name)` | Technical/context information from the separate info namespace. |
| `$each(name)` | Tree host only: evaluates the level once for every ordered value of `name`. |

Tree expansion is not inferred from scalar `$get` or `%field%`. `$each` requires
a non-empty literal field name and is rejected outside a tree-level host. Two or
more distinct `$each` fields produce their ordered Cartesian product; repeated
uses of the same field share one selected value. Missing fields and fields with
no values produce one empty selection so `$if2($each(grouping),Unknown)` works.
Evaluation fails before allocation when the configured total branch limit would
be exceeded.

## Determinism, limits, and cancellation

Compilation and evaluation perform no filesystem, database, network, locale,
environment, randomness, or wall-clock access. Nondeterministic values such as
the current playback position arrive as explicit context snapshots.

Every evaluation enforces:

- maximum source bytes;
- maximum nesting depth;
- maximum evaluated nodes/steps;
- maximum output bytes;
- cooperative cancellation;
- later, maximum expanded tree branches.

Errors include the failing source span. Batch evaluation isolates failures per
item and never reparses the program.

## Persistence and cache identity

Persist:

```text
dialect: tkfmt
dialect_version: 1
compiler_schema: integer
source: exact UTF-8 source
host: typed host identifier
name: optional user-visible name
```

Never persist only an AST. A compiled-program cache key includes the
dialect/version, compiler schema, exact source bytes, host, and function-registry
revision. An evaluation-cache key composes that value with the host's
field-provider schema and immutable track/context revision; the expression
engine cannot invent those revisions.

## Testing

The executable corpus is owned by Trackknife and records:

- dialect and schema version;
- expression host and input values;
- exact source;
- exact output or diagnostic;
- a short rationale tied to this specification.

Unit/property/fuzz coverage includes escapes, empty arguments, nested lazy
calls, missing and present-empty fields, ordered multi-values, Unicode, invalid
UTF-8, numeric overflow, division by zero, cancellation, every configured
limit, dependency extraction, and concurrent evaluation of one program.

The former foobar2000-derived cases are historical research only. They neither
define `tkfmt-1` nor count toward its completion gate.

# tkfmt — the Melody formatting language

`tkfmt-1` is the expression language used everywhere Melody turns track
metadata into text: naming layouts for rename/move and conversion, library
tree levels, ReplayGain album grouping, and metadata transformation scripts.
If you have used foobar2000's title formatting or MusicBrainz Picard's
scripts, the syntax will look familiar — but tkfmt is its own language with
its own rules, not a compatibility layer.

An expression evaluates to one UTF-8 string. There are no variables, no
assignment, no side effects: the same expression over the same track always
produces the same text. Expressions cannot modify tags or files — they only
format; mutation happens in the separate, previewed operations that consume
their output.

This page is the practical reference. The formal contract, including limits
and persistence rules, lives in
[title-formatting.md](title-formatting.md).

## Basics

Plain text is emitted as written. `%field%` inserts a tag value. `$name(...)`
calls a function. That is the whole surface:

```text
%albumartist%/%album%/$num(%tracknumber%,2) - %title%
```

For a track tagged `AlbumArtist=Daikaiju`, `Album=Daikaiju`,
`TrackNumber=3`, `Title=Escape from Nebula`, this renders:

```text
Daikaiju/Daikaiju/03 - Escape from Nebula
```

Field names are case-insensitive and may contain spaces, hyphens,
underscores, and colons. A missing field renders as the empty string. A field
with multiple values renders them joined with `; `; use the multi-value
functions below for anything else.

To write a literal `$`, `%`, comma, parenthesis, or backslash, escape it:

```text
\$  \%  \,  \(  \)  \\
```

## Truth

Conditions use string truthiness: the empty string is false, everything else
— including `0` — is true. A missing field is therefore false, and boolean
functions return `1` for true and the empty string for false. This makes the
common patterns short:

```text
$if(%album%,%album%,Unknown album)
$if2(%albumartist%,%artist%)
```

`$if2` returns its first non-empty argument, which is how fallbacks are
written — tkfmt never falls back implicitly.

## Examples

A conversion path that keeps multi-disc releases apart:

```text
%albumartist%/%album%$if(%discnumber%, CD%discnumber%,)/$num(%tracknumber%,2) - %title%
```

A ReplayGain grouping expression that treats each disc as its own album:

```text
%album%|%discnumber%
```

A library tree level that shows one branch per artist of a multi-artist
track:

```text
$each(artist)
```

A transformation script value that title-cases nothing but pads the track
number:

```text
$num(%tracknumber%,2)
```

## Function reference

Function names are case-insensitive. Arguments are full expressions and may
nest. Unknown functions and wrong argument counts are compile errors, caught
before anything runs.

### Conditionals and comparison

| Function | Result |
| --- | --- |
| `$if(cond,then[,else])` | Evaluates only the chosen branch. Missing `else` yields empty. |
| `$if2(a,b,...)` | First non-empty argument, left to right. |
| `$and(a,...)` | `1` when every argument is non-empty. Short-circuits. |
| `$or(a,...)` | `1` when any argument is non-empty. Short-circuits. |
| `$not(a)` | `1` when `a` is empty. |
| `$eq(a,b)`, `$ne(a,b)` | Byte-exact string equality / inequality. |
| `$eqi(a,b)` | Case-insensitive equality (simple Unicode case folding). |
| `$gt(a,b)`, `$gte(a,b)`, `$lt(a,b)`, `$lte(a,b)` | Integer comparison. |

### Integers

Integer parsing trims ASCII whitespace, allows one leading sign, and
otherwise requires plain decimal digits; anything else converts to zero.
Overflow and division by zero are evaluation errors, not silent wraparound.

| Function | Result |
| --- | --- |
| `$add(a,...)` | Sum. |
| `$sub(a,b,...)` | Left-associated subtraction. |
| `$mul(a,...)` | Product. |
| `$div(a,b,...)` | Integer division, truncated toward zero. |
| `$mod(a,b)` | Remainder with the dividend's sign. |
| `$min(a,...)`, `$max(a,...)` | Smallest / largest argument. |
| `$num(value,width)` | Zero-padded decimal — `$num(3,2)` is `03`. |

### Text

Positions and lengths count Unicode scalar values, not bytes.

| Function | Result |
| --- | --- |
| `$lower(text)`, `$upper(text)` | Locale-independent case mapping. |
| `$trim(text)` | Strips leading and trailing ASCII whitespace. |
| `$len(text)` | Character count. |
| `$left(text,n)`, `$right(text,n)` | At most `n` characters from either end. |
| `$longest(a,...)` | The first longest argument. |
| `$repeat(text,n)` | `text` repeated `n` times. |
| `$replace(text,search,repl[,search,repl...])` | Replacement pairs applied left to right. |
| `$pad(text,width[,fill])` | Right-pads to `width`; `fill` is one character, default space. |

### Multi-value fields and technical data

These take the field *name* as literal text, not a `%field%` value:

| Function | Result |
| --- | --- |
| `$get(name)` | All values joined with `; ` — same as `%name%`. |
| `$getmulti(name,index)` | One value by zero-based index. |
| `$join(name,separator)` | All values joined with your separator. |
| `$lenmulti(name)` | Number of values; `0` when absent. |
| `$info(name)` | Technical info: `$info(extension)`, `$info(filename)`, `$info(path)`, `$info(directory)`. |
| `$each(name)` | Library tree levels only: one branch per value of `name`. |

`$each` is what makes a track with two artists appear under both artists in
the library tree. Elsewhere it is a compile error; scalar contexts stay
scalar.

## Where expressions run

| Context | The expression produces |
| --- | --- |
| Naming layouts (rename/move, conversion) | A relative directory and a file name. Path sanitization, collision checks, and containment below the destination root happen afterwards in the planner — the expression only proposes text. |
| Conversion | `$info(extension)` resolves to the *target* format's extension, so `%title%.$info(extension)`-style names stay honest. |
| Library tree | One expression per level; `$each` expands multi-value levels. |
| ReplayGain grouping | Tracks with equal results form one album programme. |
| Metadata transformations | One derived value per track, assigned by the surrounding chain action — including per-group numbering with a group expression. |

## Errors and safety

Compilation fails closed: a typo never half-runs. Evaluation enforces source
size, nesting depth, step, and output-size limits, so a pathological
expression fails with a diagnostic instead of hanging the application. Every
error carries the span of source it came from, which the editors underline.

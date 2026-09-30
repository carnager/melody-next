# Tagging scripts as text

A tagging script is a list of steps run in order, each seeing the result of
the ones before. The **Steps** tab builds it one step at a time; the **Raw
script** tab shows the same script as text and accepts it typed or pasted
(ADR-0241). Both edit one script: a change on either is the change on both.

Each line is one step, written as a `$`-call. Values, conditions and groups
are **tkfmt-1** expressions ([tkfmt-1 reference](title-formatting.md));
capture patterns are **tkcapture-1** (ADR-0068). Nothing is executed: every
statement stands for one typed step, previewed before anything is written.

```text
$copy(DATE,ORIGINALYEAR)
$set(FMPS_RATING,$decimal(%rating%,5,1))
$if($not(%rating%),$delete(FMPS_RATING))
$delete(RATING)
```

## Steps

| Statement | Step |
| --- | --- |
| `$set(FIELD,expression)` | Format with tkfmt-1: the field becomes the expression's result. |
| `$setvalues(FIELD,value,...)` | Set values: exactly these values, as written. |
| `$addvalues(FIELD,value,...)` | Add values after the existing ones. |
| `$copy(FIELD,FROM)` | Copy another field's values. |
| `$delete(FIELD)` | Remove the field, whatever its spelling in the file. |
| `$delete(FIELD,exact)` | Remove only the field spelled exactly so in the file. |
| `$if(condition,$delete(FIELD)...)` | Remove each field when the condition is not empty. |
| `$transform(FIELD,trim)` | Trim each value; also `lower`, `upper`, `capitalize`. |
| `$splitvalues(FIELD,separator)` | Split each value at an exact separator. |
| `$joinvalues(FIELD,separator)` | Join the values with an exact separator. |
| `$removevalue(FIELD,value)` | Remove values exactly equal to this one. |
| `$replacevalue(FIELD,value,replacement,...)` | Replace an exact value with these. |
| `$number(FIELD,start,padding)` | Number the selected files in order. |
| `$numberby(FIELD,group,start,padding)` | Number again from `start` for each value of the tkfmt-1 `group`, such as `%album%`. |
| `$keepfirst(FIELD,count)` | Keep the first `count` characters of each value. |
| `$capture(filename,pattern)` | Capture fields from the filename; also `path` for the full path. |
| `$capture(formatted,expression,pattern)` | Capture fields from a tkfmt-1 result. |
| `$capture(field,FROM,pattern)` | Capture fields from another field. |
| `$deletefields(FIELD,...)` | Remove every listed field. |
| `$keepfields(FIELD,...)` | Remove every field except those listed. |

Statement names are case-insensitive. Spaces and line breaks between steps
are free; inside an argument they are part of it, as in tkfmt-1, except
around field names, which are trimmed.

## Writing text

Arguments are read the way tkfmt-1 reads a call's arguments:

- A comma separates arguments and `)` ends the step, so a literal comma or
  closing parenthesis is written `\,` or `\)`.
- In an expression, a comma at its own top level is written `\,`; commas
  inside its calls, such as `$if(a,b,c)`, are written as usual.
- In plain values -- field names, exact values, separators -- `%`, `$`, `(`,
  `)`, `,` and `\` are all written with a backslash before them.
- A field's `%...%` is one unit; an unclosed `%` is an error.

Mistakes are shown under the text with their line and column. While the text
has one, Preview and Save wait; the Steps tab keeps the last valid script.

## Picard scripts

**Paste script…** imports a small Picard-style cleanup subset (ADR-0065) into
steps. It is a migration aid, not this language: after a paste the Raw tab
shows the steps it made, in the statements above.

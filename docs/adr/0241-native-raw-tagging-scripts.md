# ADR-0241: The Raw script tab is the tagging script in Trackknife's own terms

## Status

Accepted, 2026-09-30. Supersedes the raw grammar of ADR-0070; ADR-0065's
Picard-style paste import is unchanged.

## Context

ADR-0070 gave the tagging-script editor a **Raw script** tab, but made it a
view over ADR-0065's Picard-style cleanup subset: `$set`, `$delete`/`$unset`
and `$if`, with values limited to fields and a handful of functions. Any step
outside that subset -- a copy, a split, a number, a capture, or a tkfmt-1
value using arithmetic -- turned the whole tab off with "not representable by
the raw cleanup script subset".

In practice that meant the tab was unavailable for ordinary scripts. The user
who asked to rescale a rating tag found that neither the value
(`$if($eq(%rating%,5),1.0,0.$mul(%rating%,2))`) nor a plain copy could be
written there. Two surfaces also accepted one foreign language -- Paste script
and the Raw tab -- while Trackknife's own language, tkfmt-1, had no text form
for a whole script.

ADR-0070 named when to revisit: "native transformation-chain interchange
defines a public serialized form". ADR-0072 did, as JSON; this is its text
counterpart for editing.

## Decision

- The Raw tab shows and accepts a **native script**: one statement per typed
  step, each a `$`-call, every value, condition and group a full **tkfmt-1**
  expression, every capture pattern **tkcapture-1**. The statements are listed
  in [tagging-scripts.md](../tagging-scripts.md).
- tkfmt-1 stays side-effect-free (ADR-0008). The statements are not tkfmt-1
  functions and cannot appear inside an expression; they only name the typed
  step each stands for. Nothing is executed.
- The statement layer reads calls the way tkfmt-1 does -- `$name(` opens, `)`
  closes, a top-level comma separates, `%...%` is one unit, a backslash quotes
  the next character -- so an expression is written in a statement exactly as
  it is written anywhere else, save that a comma at its top level is written
  `\,`.
- Every typed action has a statement. Export is exact: what it writes imports
  back to the identical actions, or the tab stays read-only and names the step.
  Only a step in a dialect other than tkfmt-1/tkcapture-1 can be refused today.
- Typed actions remain the saved authority (ADR-0070): the text is regenerated
  in canonical form, one statement per line, after every structured edit, and
  nothing of its spelling is stored.
- **Paste script…** stays the Picard-style import of ADR-0065. After a paste
  the Raw tab shows what it became, in native statements.
- `$delete(FIELD)` matches any spelling of the field, as the Steps tab's
  **Remove field** does; `$delete(FIELD,exact)` is the exact native name that
  a pasted Picard `$delete` produces.

## Consequences

- Every script can be read, written, copied and pasted as text, including
  scale conversions like the rating example.
- A statement name is new vocabulary (`$copy`, `$splitvalues`, `$numberby`...),
  documented in one place. Their spelling is presentation; changing it later
  changes no saved script.
- The Picard subset shrinks to what it was meant to be: a migration aid.

## Validation

- `tests/native_rule_script_test.cpp` exports and re-imports every typed
  action kind with text that means something to the statement layer in every
  position, reads a hand-written script with spacing and case, expands one
  condition over several removals, and reports mistakes at their line and
  column with nothing taken from a script that has one.
- The editor test edits a pasted script in the Raw tab and saves the typed
  result.

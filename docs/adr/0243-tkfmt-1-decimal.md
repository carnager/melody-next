# ADR-0243: tkfmt-1 gains `$decimal`

## Status

Accepted, 2026-09-30. Function-registry revision 2.

## Context

`tkfmt-1` computes with whole numbers only. Converting a rating kept on one
scale into the freedesktop `FMPS_RATING` (0.0–1.0) had to be assembled as
text -- `$if($eq(%rating%,5),1.0,0.$mul(%rating%,2))` -- which is hard to
read, easy to get wrong (without the `$if`, a 5 becomes `0.10`), and works
only for scales that happen to divide evenly into tenths.

## Decision

- `$decimal(value,divisor,places)` returns `value` ÷ `divisor` as decimal text
  with exactly `places` digits after the point, rounded half away from zero;
  `places` is 0–9. Arguments convert as integers do; division by zero,
  `places` out of range and overflow while scaling are evaluation errors, as
  they are for the integer functions.
- It is added to `tkfmt-1` rather than starting `tkfmt-2`: before this, any
  expression naming `$decimal` failed to compile, so no stored expression can
  change meaning (the rule of ADR-0008). The function-registry revision in
  the program cache key moves to 2, so no program compiled before is reused.

## Consequences

- `$set(FMPS_RATING,$decimal(%rating%,5,1))` replaces the text assembly; any
  scale converts the same way (`$decimal(%rating%,100,1)` for 0–100).
- Decimals are produced, not computed with: `$decimal` output fed back into
  integer functions converts to zero, as any non-integer text does.

## Validation

Corpus cases `decimal.scale`, `decimal.rounding` and `decimal.sign`, and
evaluator tests for the most negative integer, nine places, division by
zero, `places` out of range and overflow.

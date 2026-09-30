# ADR-0244: Tagging scripts convert a player's rating into FMPS_RATING

## Status

Accepted, 2026-09-30. Schema 48.

## Context

Trackknife keeps a track's rating in `FMPS_RATING` (0.0–1.0), which each
format's writer spells its own way -- and in MP3 accompanies with a
popularimeter (ADR-0237). Other players leave ratings in tags of their own on
scales of their own: 1–5 stars, 0–10, 0–100, sometimes with halves.

Moving such a rating into `FMPS_RATING` took a hand-written expression per
scale (ADR-0243's `$decimal` made that shorter, not easier), and could not
read half stars at all. The engine, meanwhile, already converts a plain
`RATING` tag on a chosen scale when it reads files (`rating_from_plain`).

While building this, copying from a freeform source field -- a player's own
tag is usually one -- turned out to find nothing: freeform fields are kept by
their exact native name, and the copy step looked only among conventional
ones, so it silently emptied its target.

## Decision

- A tagging-script step, **Convert rating to FMPS_RATING**, takes a source
  field and the scale its rating is kept on: 0–5 stars, 0–10 or 0–100. It
  converts with the engine's own `rating_from_plain`, so the step and the
  library agree on every value, halves included: 4 of 5 becomes `0.8`, 3.5
  becomes `0.7`. The target is always `FMPS_RATING`.
- A missing, unreadable, out-of-scale or zero ("unrated") source leaves
  `FMPS_RATING` as it was. The step previews like any other; Apply writes it,
  and the writers add each format's spelling and the MP3 popularimeter.
- Stored as action kind 20: `argument` the source field, `integer_argument`
  the scale's top (5, 10, 100). Schema migration 48 rebuilds
  `metadata_transformation_actions` with the widened kind check, copying
  every row, in one transaction.
- Native JSON (ADR-0072) gains `convert_rating` with `target_field`,
  `source_field` and `scale` (5, 10 or 100). A reader without it fails closed
  on the unknown kind, as version 1 requires, rather than misreading it.
- The raw script (ADR-0241) writes it `$rating(FIELD,5)`.
- A copy, and this step, read a freeform source by its exact native name, as
  a capture's field source already did.

## Consequences

- Moving another player's ratings is one step, chosen from a list, with the
  scale asked for -- no expression to write.
- A database migrated to 48 is refused by an older build with the usual
  "created by a newer application version"; engine and window upgrade
  together.
- Scripts that copied from a freeform field now copy its values, where they
  previously emptied the target.

## Validation

- `tests/metadata_transformation_test.cpp`: whole and half stars, a rating
  already present, unrated, out-of-scale, unreadable and absent sources, the
  0–100 scale, refusal of another target or no scale; and a copy from a
  freeform source.
- `tests/list_repository_test.cpp`: the step saves and loads through schema 48.
- `tests/metadata_transformation_interchange_test.cpp` and
  `tests/native_rule_script_test.cpp`: JSON and raw round trips.

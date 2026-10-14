# ADR-0283: List views show when tracks came, fold their groups, and style their cells

## Status

Accepted, 2026-10-14. Extends ADR-0282 (list views).

## Context

List views (ADR-0282) group and show what a list's rows hold. Three things a
large list wants were missing: when its tracks came into the library -- a
"recently added" view needs it, and the engine knows it but did not send it
with a list's rows; folding a group away, so a list of two hundred albums is
two hundred lines; and saying more than text -- a track never played shown
quietly, a missing tag shown as a warning.

## Decision

- **When a track came.** A track's description (`list.describe`'s
  `library`) says when it, and its album's newest track, came into the
  library: `added`, `album_added`, Unix seconds (protocol level 12). An
  older engine says neither; they read as not known. Expressions read them
  as `added` and `albumadded` (local date and time, which sorts as it reads)
  and, as `tkq-1` has them, `dayssinceadded` and `albumdayssinceadded`.
- **More starting points ship** *(withdrawn 2026-10-14 with those of
  ADR-0282: two views ship, and a list using a retired one is drawn by the
  one in its place)*, read-only and copyable: Grouped by year
  (album subgroups), Grouped by decade, Grouped by folder, Classical
  (composer, work), Tag check (missing genre, ReplayGain and MusicBrainz id,
  as warnings), Grouped by quality (codec, bits, rate) and Recently added
  (albums by when they came; sorted by Added, newest first). In the header
  menu the four presentations and the user's own views stay at hand; the
  starting points are one menu down.
- **Groups fold.** The mark at a group header's right end folds the group
  to its header and unfolds it again; the header menu folds or unfolds every
  group. A folded group's rows stay in the list -- played, and selected
  with the group -- and are not drawn. Folds are kept with the tab, by group
  key, across restarts; a group whose key changes opens again.
- **Styles.** A view has an optional row style expression, and each
  expression column an optional style expression of its own. They answer
  style words -- `quiet`, `accent`, `warning`, `bold`, `italic` -- separated
  by spaces or commas; a word not known is ignored, so a newer one reads as
  nothing. A cell's colour is its own where it gives one, else its row's;
  bold and italic add up. The colours are the theme's (placeholder text,
  highlight) and one warm warning colour, not chosen per view, so a view
  reads alike in light and dark. The playing row keeps its own colour, and
  the rating its stars'. Listening history dims tracks never played; Tag
  check marks what is missing.

## Consequences

- Style words, not colours, keep views portable between themes; a view
  wanting a colour of its own would need a further decision.
- A folded group keyed by an expression that changes -- a label retagged --
  opens again; that is the honest answer for a group that is not the same.

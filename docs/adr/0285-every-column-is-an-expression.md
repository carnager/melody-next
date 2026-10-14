# ADR-0285: Every column is an expression

## Status

Accepted, 2026-10-15. Amends ADR-0282 (list views), where built-in columns
were drawn only as Trackknife draws them.

## Context

A list view's built-in columns -- Artist, Title, Length and the rest --
showed no expression in the editor and could not be changed: only their
second line was the view's. To show a two-digit track number, or the year
of a date, the user hid the built-in column and added an expression column
in its place, losing what the built-in one does (its sorting, the playing
mark). The user asked for the built-ins to be visible and editable, as every
column is in foobar2000. Product decision (2026-10-15): every column is an
expression; what only Trackknife can do stays with the column whatever it
shows.

## Decision

- **A built-in column has the fields every column has**: a header, what it
  shows, a second line, a sort, a style and an alignment. Each is empty
  until given: an empty header is its usual name, an empty "shows" is
  Trackknife's own drawing.
- **The editor shows a built-in column as its expression**: Artist as
  `%artist%`, Length as `%length%`, and so on, ready to change. Left as it
  was, nothing is saved for it -- it stays Trackknife's own drawing, with
  its fallbacks (the file name for an untitled track). "Reset to default"
  clears what was given. The rating and the cover have no expression that
  draws them as they are; their "shows" starts empty.
- **What only Trackknife does stays with the column**: the playing mark on
  the title, clicking the rating to rate, the column's identity in layouts.
  A built-in column keeps its own sorting -- the number by disc first, the
  length by milliseconds, history by the engine's -- unless given a sort of
  its own; what it shows does not change how it sorts.
- **Saved with what it was given, and only that**: a built-in column's
  title, display, sort, style and alignment are written when set. Views
  saved before read as they were.
- **Plain columns ships in the user's arrangement**: Artist, # (two
  digits), Title, Date (the year), Album, Rating, Length -- the number,
  year and length right-aligned, through the built-in columns' own
  expressions rather than replacements.

## Consequences

- A view can change any column without losing its behaviour, and the
  shipped views show what an expression does in the editor itself.
- A rating shown as an expression is text; it is still clicked to rate.
- Built-in columns evaluate on the same worker as expression columns; a
  view whose built-ins have no expressions costs nothing more.

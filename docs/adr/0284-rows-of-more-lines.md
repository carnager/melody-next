# ADR-0284: List rows of more than one line

## Status

Accepted, 2026-10-14. Amends ADR-0027 (cells are single-line) for list views
(ADR-0282).

## Context

A modern player's list says more per row than one line holds: the title,
and beneath it the artist and album, smaller. ADR-0027 kept every cell to
one line, which is what keeps a list of 100,000 rows fast -- every row the
same height, so where a row is follows from its number, with nothing
measured. Rows that grow to fit their text would need every row measured,
and again on every resize and tag change, and a list drawn by something
other than Qt's table.

## Decision

- **A view says how many lines its rows have**, one to three. Every row of
  the view is that tall -- the first line as before, each further line
  shorter -- so nothing is measured and a list scrolls as it did.
- **Any column may have a second line**: an expression, built-in columns
  included (the Title with `%artist% · %album%` beneath it). It is drawn
  beneath the cell's first line, smaller and quieter, aligned as the cell
  is; in rows of three lines it wraps over the two, and is cut where it
  does not fit.
- The selection's tint and stripes lie under every line; the playing mark,
  an album's cover and the hairlines are where they were. Group headers
  stay one line.
- Each cell's first line is still single-line, as ADR-0027 says; what is
  amended is that a row is one line.
- **Two-line tracks ships**, a third view beside albums and plain columns:
  the title with its artist and album beneath, the year with its genre, the
  length with its format (`flac · 16/44.1`), the rating with how often it
  played -- no column for what a second line carries.
- A column may say how wide it starts. A tab keeps the widths it was given;
  choosing a view gives its columns the widths the view says.

## Consequences

- A view of two-line rows shows half as many tracks per screen; it is the
  user's choice, per view.
- Rows that grow to fit their text, or free layouts within a row, remain
  out of reach: they need a list drawn by its own layout code.

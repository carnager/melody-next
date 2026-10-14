# ADR-0282: List views are columns and groups of tkfmt-1 expressions

## Status

Accepted, 2026-10-14. Extends ADR-0027 (versioned track-view layouts) into the
`TrackViewDefinition` that `docs/ui-workspace.md` specifies, as ADR-0254 did
for the library tree. Fills "arbitrary expression-defined columns/grouping" in
the feature matrix.

## Context

A list tab draws ten fixed columns, groups by album artist, album and date,
and offers four presentations (side artwork, header artwork, plain columns,
compact queue) with per-tab column order, visibility and width. A foobar2000
user expects to say what a column shows -- `%label%`, `$if(%rating%,…)`,
`[%discnumber%.]%tracknumber%` -- and what a list groups by and its headers
say. `ui-workspace.md` has required exactly this since the start, and the
client already holds every tag of every row (`list.describe` sends them all),
so nothing new has to cross the protocol.

## Decision

- **A list view** is a name and a versioned definition (dialect `tkfmt-1`,
  version 1):
  - **artwork**: beside a group's rows, in its header, or none;
  - **columns**, in order, each shown or hidden: the built-in ones -- Artist,
    #, Title, Album, Date, Length, Rating, Play count, Last played, and the
    artwork gutter -- and **expression columns**, each a title, a display
    expression, an optional sort expression (else the display), an alignment
    and whether it takes up spare room;
  - **grouping**: none; the album, as today (album artist, album and date,
    discs within); or an **expression grouping** -- a group-by expression,
    rows grouped in runs of equal value in the list's order, a header with a
    title and a details expression, and an optional subgroup with its own key
    and label expressions, shown where it changes inside a group of more
    than one.
  Cells, headers and subgroup labels stay single-line (ADR-0027).
- **The built-in columns stay built in.** They are drawn as now, with no
  expression to evaluate, and keep the album view's rules: inside an album the
  header says album and date, and the artist shows where it differs. Under an
  expression grouping those rules are off -- the group is not an album.
- **Expressions read the row as the engine's formatting does.** Tags by name;
  host fields `length` (m:ss), `path` and `rating` (1-10);
  technicals and the file through `$info(...)` -- `codec`, `samplerate`,
  `bitspersample`, `channels`, `bitrate`, `filename`, `filename_ext`,
  `directory`, `extension`. A group header also reads `group_tracks` and
  `group_length`, of its whole group.
- **Listening history is read too**, under the names `tkq-1`'s `HISTORY(...)`
  uses: `playcount`, `lastplayed` (local date and time, which sorts as it
  reads), `dayssinceplayed`, and the album's of each. A view that reads one,
  or shows the Play count or Last played column, has its list's history
  asked of the list's engine (`catalogue.history_facts`) before those rows
  are evaluated -- the whole list, not the rows on screen, so it can sort and
  group by it. The engine says when it records a listen (`catalogue.listened
  {path}`, protocol level 11), and that file's history is asked for again.
- **Evaluated off the window's thread.** Each expression is compiled once per
  view; a list's rows are evaluated in batches on a worker and the results
  kept with the rows, evaluated again only for rows that change. Until a
  batch lands, its cells are blank and its rows ungrouped. The budgets of
  `ui-workspace.md` apply: no expression is evaluated while painting.
- **Shipped views** replace the four presentations, each as it was: Albums
  (artwork beside), Albums (artwork in headers), Columns, Compact queue. Five
  starting points ship beside them: Grouped by artist (albums as subgroups),
  Grouped by genre, Grouped by label (with the catalogue number), Audio
  details (codec, rate, bits, bitrate, file) and Listening history (rating,
  play count, last played, days since). They are read-only and can be
  copied.
- **Views live in Trackknife's settings** (`lists/views`), not in an engine:
  they are how this user wants lists to look, whichever engine they come
  from. The Android app keeps its own album layout.
- **A tab uses a view by its id**; column widths stay the tab's own. Showing,
  hiding or moving a column changes the view, and so every tab that uses it;
  done to a shipped view, it makes a copy, named after it, which this tab
  then uses.
- **Clicking a column header sorts the list** by that column, ascending;
  again, descending -- by its sort expression, the same natural, stable order
  as Sort list. It is the list's real order, an undoable edit (ADR-0127), not
  a view order, so playback and grouping follow it. The built-in columns sort
  by what they show (# by disc and track number).
- **The editor** (header menu: Edit view…, New view…) edits the name,
  artwork, grouping and columns, with a live preview of the current tab's
  first rows and each expression's error beside it. A view with an error is
  not saved.
- **Migration.** A saved ADR-0027 layout becomes its presentation's shipped
  view and keeps its widths. A tab whose columns differ from its
  presentation's -- shown, hidden or moved -- gets a copy with them, one per
  distinct arrangement, named "<view> (customised)", so nothing a user chose
  is lost. A newer or unreadable layout falls back visibly without
  overwriting its bytes, as before.

## Consequences

- `LocalListModel` has the built-in columns and, after them, the view's
  expression columns; the group key, header and subgroup are roles the model
  answers from what it evaluated, which the shared delegate prefers to its
  album rules.
- A view reading history waits for the engine's answer before its rows are
  evaluated; an engine that cannot say shows them without it.
- Sorting a list by a column is a list edit with undo, never a hidden view
  state.
- Views are per computer. Sharing them with the Android app or between
  machines would need them stored by the engine; not planned.

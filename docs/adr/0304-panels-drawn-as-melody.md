# ADR-0304: Panels drawn as Melody draws its own

## Status

Accepted, 2026-10-20. Extends the panels of ADR-0291 and ADR-0303.

## Context

- **A panel looked like a form, not like Melody.** Its parts -- headings,
  paragraphs, items, checkboxes, buttons -- were plain widgets. The
  listening room's queue was items with a "▶" typed into the text, its
  listeners a paragraph of names, its status a sentence, its Stop room a
  button as loud as Start; nothing showed what plays the way Melody's own
  lists do.
- **Styling is Melody's to do.** A plugin that sent its own CSS or colours
  would clash with every colour scheme and look different in every panel.
  A plugin should say *what* something is; Melody draws it.

## Decision

1. **Parts that say what they are**, drawn by Melody in its own colours:
   - `{"track": TITLE, "artist", "detail", "cover", "url", "playing",
     "buttons"}` -- a track's row as Melody's lists draw one: the title
     over the artist and detail, a small cover when `cover` names an image
     on this computer, what plays tinted in the accent, its buttons at the
     right;
   - `{"status": TEXT, "detail", "live"}` -- a status line, a dot in the
     accent while it is live;
   - `{"people": [NAME, …]}` -- people as chips, each with an initial in a
     colour of its own, the same colour the listening room's web page
     gives that name.
2. **A button has a role**: `"role": "primary"` (the accent: the one to
   press), `"normal"`, or `"quiet"` (flat: Stop, Leave). On a row, a quiet
   button is a bare glyph shown only under the pointer, keeping its place
   so the row does not shift. ADR-0303's `"primary": true` stays the
   primary role.
3. **The parts already there are restyled**, not changed: a toggle is a
   switch at the right of its label, a section's heading is in spaced
   small capitals over a lighter rule.
4. **No styling from plugins.** No CSS, no colours, no fonts: the parts are
   all a plugin has, so every panel looks like the rest of Melody under
   every colour scheme.

## Consequences

- The official Listen together panel uses them: its status, the party
  switch, Stop quiet, the queue, the played and the listeners as tracks and
  people.
- Every old part still works as it did.

## Verification

- `plugins`: tracks, statuses, people and roles read, malformed ones
  refused; `primary: true` read as the primary role; the Listen together
  panel answers them.
- `bench-main-window`: the Listen together panel drawn -- what plays
  marked, the switch on, the people, the host's remove hidden until the
  pointer is on its row.

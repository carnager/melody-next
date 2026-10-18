# ADR-0294: Up Next under the lists; Panels for the rest

## Status

Accepted, 2026-10-18. Changes ADR-0292's side panels: Up Next is no longer
one of them.

## Context

- **Up Next was a side panel**, tabbed with Lyrics and plugin panels since
  ADR-0292. But it is not like them: it is worked in -- reordered, cleared,
  dropped on, returned from -- and decides what plays next, from the list
  that plays. The others are read: they follow what plays.
- **Two columns of panels side by side look cluttered**, and tabbing Up Next
  with Lyrics hid one of them behind the other.

## Decision

1. **Up Next is a section under the lists**, in a vertical split with the
   list tabs: its bar (Up Next, whose, how many wait, close), its tracks,
   its toolbar and the way back to the list. Its height is kept
   (`up-next/height`), and whether it is open (`up-next/visible`). The
   header's Up Next button with its count and drop target, the Playback
   menu entry and Ctrl+Shift+U open and close it.
2. **The side column holds the reading panels only** -- Lyrics, plugin
   panels -- as tabs (ADR-0292).
3. **Panels**, a button in the header beside Up Next, hides every open side
   panel at once and brings back the same ones (`panels/open`; Lyrics when
   none were); it shows pressed while one is open.

## Consequences

- One column on the right at most; Up Next where its tracks come from.
- The list loses the height Up Next takes while open; the divider is the
  person's.

## Verification

- `bench-main-window`: Up Next under the list tabs, opened and closed, its
  height kept; not a dock among the side panels; Panels hiding the open ones
  and bringing the same back, as tabs.

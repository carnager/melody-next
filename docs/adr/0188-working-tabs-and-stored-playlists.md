# ADR-0188: Working tabs and stored playlists are different things

## Status

Accepted, 2026-09-19. Corrects ADR-0187, which collapsed both into one;
restores the client-owned tabs of ADR-0181 with a clearer role.

## Context

ADR-0187 made every server list an MPD stored playlist. That removed a
duplicate concept but also removed something users actually wanted: a
scratch list you assemble in the tab area, edit freely, play, and throw
away — the server-side equivalent of a local working list. Stored
playlists are the opposite: long-term, curated, shared with every client,
and edited through server round trips.

Both are useful, and they are not the same thing.

## Decision

Two server-side list kinds, with distinct jobs:

- **Working tabs** — client-owned, temporary lists of server tracks. Built
  by dropping tracks onto the tab strip or "Copy to tab > New tab…",
  edited entirely client-side (add, remove, reorder, rename), persisted
  with the workspace so they survive restarts, and rendered from metadata
  snapshots so they stay legible offline. Enter or double-click plays the
  tab from that row by replacing the queue — the same gesture the MPD
  Queue tab has, which is what "play it like the queue" means.
- **MPD stored playlists** — long-term, server-owned lists in the sidebar,
  opened as tabs, edited as server round trips (ADR-0129), and played as
  playback contexts (ADR-0187) so switching between them resumes each one.
  "Add to playlist" writes here, with a New playlist… entry that creates
  one and opens its tab.

Each destination appears exactly once in the context menu: **Copy to tab**
for working tabs, **Add to playlist** for stored playlists.

Tracks move between the two freely: any MPD surface can copy a selection
into a working tab or a stored playlist, and dropping rows on the tab strip
builds a working tab from them (dropping onto an existing working tab
appends).

## Consequences

- Three list concepts exist, but each answers a different question: local
  lists are files on this machine, working tabs are scratch server
  selections, stored playlists are the library's curated lists.
- A working tab needs no server object, so it can be assembled and
  reordered while the server is busy or gone; a stored playlist is the
  thing that other clients see.
- Promoting a working tab to a stored playlist is "Copy to server list"
  from within it; the reverse is dropping a playlist tab's rows onto the
  tab strip.

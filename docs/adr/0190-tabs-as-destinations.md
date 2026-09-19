# ADR-0190: Every MPD tab is a destination, and the queue tab edits the stash

## Status

Accepted, 2026-09-19. Extends ADR-0188 (working tabs and stored
playlists); depends on the Melody `melody_context queue*` edits.

## Context

ADR-0188 gave every MPD-side surface the same standing: each tab is a
list, and one of them is the active queue. The Queue tab therefore shows
the *queue context* — which, while a playlist or working tab is playing,
is the list the server holds stashed rather than the materialized one.

That left edits behind. Everything aimed at "the live queue" — the
library menu's append/insert/replace, drops onto the queue view, delete,
crop, clear, reorder — still addressed the MPD queue, which at that
moment is the materialization nobody is looking at. Tracks went
somewhere invisible: the command succeeded, the audio even changed, and
no tab showed the result. Working tabs had a second gap: they accepted
no drops at all, so the library could not feed them by drag.

## Decision

**The queue context is editable.** Melody gains
`melody_context queueadd/queuedelete/queuemove/queuereplace`, addressing
the same list `queueinfo` reports. The client routes every queue
mutation through them whenever a stash exists, so the Queue tab edits
the list it displays. `queuereplace` also switches back and plays:
replacing the queue means the queue is this list now.

**Tabs are destinations.** A selection of server tracks goes to a tab,
not to "the queue":

- The library's two direct actions name the visible tab — *Add to
  \<tab\>* and *Replace \<tab\>* — because the tab you are looking at is
  the obvious target. A local list tab cannot hold server rows
  (ADR-0058), so there the Queue tab stays the default.
- *Send to tab* lists every MPD-side tab (queue, working tabs, open
  playlist tabs), each offering Add / Insert next / Replace, for aiming
  somewhere else without switching tabs first.
- Working tabs accept drops from every server-track surface: the library
  tree, the queue, playlists, searches and other tabs.

Each destination applies the placement in its own terms: the queue
through the context commands, a working tab in client memory, a stored
playlist through `playlistadd`/`playlistclear`.

## Consequences

- An edit always lands where the user aimed it, and stays visible.
- The queue tab keeps working unchanged when nothing is stashed: the
  ordinary MPD commands already address the queue context then, and the
  context edits ACK on such servers.
- Plain MPD servers keep the old behavior throughout — no stash exists,
  so no routing happens.
- `--debug` traces the pieces this depends on (context state per
  snapshot, command results, what the workspace restored), because the
  failure mode here was invisible by construction.

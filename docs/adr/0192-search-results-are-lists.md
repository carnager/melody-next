# ADR-0192: A committed search is a list

## Status

Accepted, 2026-09-19. Supersedes ADR-0140's committed search tabs.
Extends ADR-0191.

## Context

ADR-0140 gave a committed search its own tab kind: a query-keyed,
session-only snapshot of the hits, held in the client and read-only.
Under ADR-0191 that is the last client-owned list left — and the odd one
out in the tab strip, where every other tab is a server list that can be
reordered, edited, played and added to. A search result is exactly the
kind of thing worth keeping and working on; making it the one tab you
cannot touch was a distinction without a reason.

## Decision

Committing a search writes its hits to the server as a working list
(ADR-0191) named after the query, and opens it as an ordinary tab. The
`MpdSearchTab` kind is gone, with its menu, its close path, and its
snapshot model.

- Searching the same thing again replaces that list's contents rather
  than appending, keeping ADR-0140's "recommitting refreshes in place".
- A search list is a working list in every other respect: closing it
  deletes it, "Keep as playlist" promotes it, and every list gesture
  applies.
- The live search panel is untouched — it is a preview, not a list.
- Disconnected there is nowhere to put the hits, so no tab appears and
  the status bar says so. This is the honest consequence of lists living
  on the server.

## Consequences

- One kind of MPD-side tab remains: a stored playlist, scratch or
  curated. Tab handling, persistence, and the destination menus stop
  branching on a third case.
- Committing a search now writes to the server, where before it cost
  nothing. Searches you do not keep are a tab you close, which deletes
  the list.

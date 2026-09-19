# ADR-0191: Working lists live on the server

## Status

Accepted, 2026-09-19. Supersedes ADR-0188's client-owned working tabs and
ADR-0181's client-owned server list tabs; the ADR-0190 destination model
carries over unchanged.

## Context

ADR-0188 made a working tab a client-owned list: rows held in the
client, persisted in its workspace database, materialized into the
server queue to play. That is not how MPD works, and the design produced
exactly the failure the split invites. While such a list was the active
queue, `mpc add` (or any other client) appended to the queue — correct,
and invisible here, because the tab was rendering its own copy. The next
edit in the tab then overwrote what the other client had added. Two
copies of one list, with no rule that could keep them equal.

Each fix made it worse in shape: the queue tab needed server-side stash
edits, then the working tab needed a label to recognize itself in the
queue, then a resync command to push its edits back. All of it existed
to reconcile a duplicate that should not exist.

MPD already has a list concept: the stored playlist. The only thing it
lacks for this purpose is a way to say "this one is a scratch list, show
it as a tab."

## Decision

**Every list lives on the server.** A working tab is an ordinary MPD
stored playlist carrying a Melody `scratch` flag.

- **Creation** writes to the server: `playlistadd` the selection, flag it
  scratch, open its tab. Dropping a selection on the tab strip does the
  same with a generated name.
- **Editing** is the stored-playlist command set — `playlistadd`,
  `playlistdelete`, `playlistmove` — the same commands playlist tabs
  already used. The client keeps rows only as a render cache.
- **Playing** is `melody_context play <name>`, the path playlist tabs
  already take: the queue it displaces is stashed, not destroyed.
- **Presentation** is the only thing the flag changes: scratch lists
  appear in the tab strip, curated playlists in the sidebar. Both are
  stored playlists to every other client.
- **Closing** a working tab deletes its list, with "Keep as playlist"
  offering promotion instead — clearing the flag, contents untouched.
- **Plain MPD** servers have no flag, so a list created there is simply a
  playlist; nothing else changes.

Legacy client-owned documents are dropped at startup rather than
migrated, as agreed.

## Consequences

- One copy of each list, so a stock client's add is visible everywhere at
  once and cannot be overwritten by the next edit here.
- The scaffolding that existed to reconcile the duplicate is gone:
  `melody_context tracks`, `label` and `resync`, the ad-hoc context
  switch, and the queue-tab mirroring built on them.
- `melody_context queue*` edits stay: the Queue tab still shows the
  stashed queue while another list plays, and still has to edit it.
- A scratch list is visible to other clients as a playlist. That is the
  honest consequence of using MPD's own list concept, and the price of
  not inventing a second one.

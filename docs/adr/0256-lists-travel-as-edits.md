# ADR-0256: Lists travel as edits, and play by reference

Status: Accepted (2026-10-03)

Amends ADR-0233 (its `list.save`-only protocol and its sync) and the Phase 2
text of `docs/unified-engine.md` that has playing a track hand the engine
"the whole list".

## Context

ADR-0233 put every list on its engine, but the window still moves lists as
wholes:

- **Sync.** `EngineListSync` fingerprints each list and, whenever the
  fingerprint differs from what the engine was last sent, sends all of it
  with `list.save`. A reconnect forgets every fingerprint, so every list is
  sent again.
- **Play.** Starting a track sends the whole list as the engine's queue
  (`playback.replace_queue`), although the engine holds that list already
  and has `list.play {id, entry}`.
- **Edits to the playing list** send the whole queue again
  (`Workspace::syncEngineQueue`).

A search of 5,000 tracks kept as a tab on gemenon was a 1.25 MB `list.save`
line. The engine's line limit is 1 MiB; it dropped the connection without a
word, the window reconnected, re-sent ReplayGain, re-attached, saved the
lists again and sent the same line: a loop for as long as the tab was open
(2026-10-03). Raising the limit only moves the wall, and a window resending
megabytes it has already sent is wrong at any size.

## Decision

**A list is sent whole once, when it is made, and then only as edits.** The
engine already holds it; what it needs to hear is what changed.

**`list.edit {id, revision, ops}`** applies operations in order to the list
at `revision` and answers its new summary. A `revision` that is not the
list's current one is refused as `conflict`; the client reads the list and
works out its edits again against what it found. Operations:

| Op | |
| --- | --- |
| `{"remove": [entry…]}` | those entries go |
| `{"insert": [item…], "after": entry?}` | new items, placed after `after`, or first when it is null |
| `{"move": [entry…], "after": entry?}` | existing entries, taken out and placed after `after` in the order given |
| `{"update": [item…]}` | items replaced in place by entry: a relocation, a re-read tag, a new ReplayGain |

An entry or anchor the list does not hold refuses the whole edit as
`not_found`; nothing is applied.

**The window works out edits by identity.** For each list it keeps what the
engine acknowledged: the entries in order, each with a fingerprint of what
the engine stores for it. A sync compares the tab's rows with that: entries
gone are removed; entries new are inserted; entries whose relative order
changed are moved -- those outside a longest run that kept its order, so
dragging three rows moves three; entries whose fingerprint changed are
updated. Operations are sent in batches that stay well under the line limit,
each against the revision the previous one produced. A batch that fails
leaves the acknowledged state where the last one left it, so the next sync
carries on from there: a dropped connection never leaves the window
believing the engine has what it has not.

**A new list** is a `list.save` of at most one batch of items, then inserts.
A list made from many tracks is made of many small messages, never one large
one.

**Reconnecting sends nothing by itself.** The window compares the engine's
`list.all` revisions with the ones it acknowledged. The same revision: in
step, nothing to do. A newer one: changed elsewhere, read and handled as
ADR-0233 says. Missing: the engine lost it, or never had it; it is made
again. Only the third sends items.

**Play by reference.** Starting a track is `list.play {id, entry}`, after
any edits to that list waiting to be sent. The engine builds the queue from
its own copy, with the same entry identities. A list item therefore carries
what a queue entry needs and the engine cannot read for itself: the
ReplayGain override from a sidecar or a CUE sheet (ADR-0139/0141), and the
album artist and date the album shuffle groups by.

**The queue follows the list it was played from.** An edit to the list the
queue came from (ADR-0253) is applied to the queue too, in the same request.
The window no longer pushes the queue when the playing list changes; it
edits the list, as it would any other.

**Searches kept as tabs are made on the engine.** Keeping a library search
asks the engine to make a working list from the query; the tracks never
travel from the window. The window reads the list it was given, as for any
list.

**The line limit stays at 1 MiB**, and the engine answers a longer line as
the call it was (`limit_exceeded`) instead of dropping the connection. No
request the window makes comes near it.

## Not decided here

- Reading a long list's rows in pages instead of whole. `list.get` of 66,000
  entries is one large answer; answers are not limited, and the window shows
  every row. Whether a tab should hold only what is on screen is step 3d's
  question.
- Edits of saved lists by two clients at once beyond what ADR-0233 offers
  (reload theirs, keep mine, save a copy).

## Consequences

- No message the window sends grows with the length of a list, except the
  batches of a list being made, which are bounded.
- A reconnect costs one `list.all`.
- `playback.replace_queue` stays for clients that hold no lists of their own
  (the phone's queue of an album, `melody-cli`), not for the window.
- `engine_list_items` gains the ReplayGain override, album artist and date
  (migration 0050).

## Verification

- Repository tests for every op, its refusal, and the revision.
- The diff: removals, inserts at the start, middle and end, a drag, a sort,
  updates, and that applying the ops to the old list gives the new one, over
  randomised lists.
- An engine test: an edit of the playing list changes the queue the same
  way, and the playing entry keeps playing.
- A window test: a 20,000-row tab is made in batches under the limit;
  dragging a row sends one move; a reconnect sends no items; playing sends
  `list.play`.

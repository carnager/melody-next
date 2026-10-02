# ADR-0253: A list can continue with a dynamic playlist's rule

## Status

Accepted, 2026-10-02. Builds on ADR-0195 (dynamic playlists), ADR-0215
(listening-history queries) and ADR-0220 (the engine owns playback). Fills the
queue replenishment ADR-0195 left as a follow-up.

## Context

A list that plays to its end stops. What was wanted is the radio's habit: as
the list runs out, more of what the listener likes follows, chosen by a rule
already written as a dynamic playlist. It has to happen where playback is --
in the engine -- because the point is music that does not stop, and the
window may be closed.

Dynamic playlist rules live in the window's settings; the engine has never
seen them. The engine's queue does not know which list it came from. Both had
to change.

## Decision

- **Per list.** A list may *continue with* one dynamic playlist whose source
  is a `tkq-1` rule. Lists without it end as before; an ordered album list is
  never extended unasked. The tab carries a mark and names the rule.
- **The engine keeps a copy of the rule** for each continued list -- its
  identity, name and query -- in its key/value store
  (`list-continuation.v1`), so it works with no window open. The window sends
  the copy when a continuation is chosen and again when the rule is edited;
  a deleted rule, or one no longer a `tkq-1` rule, ends the continuation.
  Last.fm-sourced rules are not offered: the engine cannot fetch Last.fm
  here yet, and a continuation that stops when the window closes would fail
  at exactly the moment it exists for.
- **The queue knows its list.** `playback.replace_queue` and `list.play`
  carry the list's identity; the engine keeps it with the saved queue.
- **When.** As a track starts and nothing would follow it -- the list ends
  after it under the modes and asks in force -- the engine appends **10**
  tracks. Repeat never ends a list, so it never continues; single stops on
  purpose, so it does not continue either.
- **What.** The rule, run against the engine's own library, without tracks
  already in the queue and without tracks played in the last 7 days:
  `(rule) AND (HISTORY(dayssinceplayed) MISSING OR HISTORY(dayssinceplayed)
  GREATER 6)`. Should that leave nothing, the rule alone, still without what
  is queued. Ten are drawn at random from what matches.
- **Into the queue, not the stored list.** The engine appends to the queue
  without resetting the play order; the window, as for any change the engine
  makes to its queue, takes the new entries into the playing tab and saves
  the list. A window that was closed takes them in when it reattaches. The
  engine never writes the list document itself, so its continuation can
  never collide with an edit the user is making.
- **Ordinary entries.** What was added can be removed, moved, rated; ending
  the continuation keeps what is already there.

## Consequences

- A continued list plays on indefinitely, from the library, with no window.
- The window's dynamic playlist rules gain a second home; the window keeps
  the engine's copies in step, and an engine that was unreachable when a rule
  changed keeps the old copy until the window next reaches it.
- Last.fm-sourced continuation is a later step, when the engine can fetch
  Last.fm itself.

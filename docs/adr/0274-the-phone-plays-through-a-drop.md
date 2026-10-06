# ADR-0274: The phone plays through a drop

## Status

Accepted, 2026-10-06. Amends ADR-0228 (output agents) and the phone's
speaker of ADR-0230; builds on ADR-0271's buffer of minutes.

## Context

Out of doors, through a web proxy, the phone stopped playing whenever it was
locked, and played again once it was opened. A log kept on the phone showed
why: locking it dropped the connections -- Android ends every connection of
a network that is replaced ("Software caused connection abort"), and a VPN
such as Proton's is rebuilt when the phone changes networks -- and the
phone's speaker, on losing its connection, paused what it played and waited
for the engine. It had more than five minutes buffered. Through the person's
own WireGuard, which roams without rebuilding, the connection had never
dropped.

The same log showed Android ending the app in the background for CPU use:
the speaker looked at its player ten times a second, playing or not.

## Decision

- **The phone plays on** when its speaker's connection drops, from what it
  has buffered, and makes the connection again; the app stays running while
  it does. Paused from the lock screen meanwhile, it pauses itself, as the
  engine cannot be asked.
- **It comes back saying what it plays.** `agent.register` carries `report`,
  the speaker's report as it sends them, beside the process's `instance`.
- **The engine takes it at its word** when it is the same process, still
  holding what the engine gave it, and on the entry the engine plays or the
  one armed after it: what it knew of the speaker is kept, the report is
  adopted -- a handover made while away moves the engine on, as any
  report's does -- and nothing is loaded again. Anything else -- another
  process, nothing held, another entry chosen while it was away -- is taken
  up where it was, as before.
- **Idle, the speaker waits for a change** rather than looking: reports
  four times a second while playing, otherwise on a change, or once a
  second.
- **The app keeps a log** of its connections, its playing, its networks and
  why Android last ended it, shared from Settings › Diagnostics: the
  system's own reaches back a minute.

## Consequences

- Protocol level 8. An older engine takes a returning phone up where it
  dropped: the music goes on, then jumps back to that place.
- A phone away longer than its buffer stops when the buffer ends, and the
  engine takes it up where that left it once it is back.

# ADR-0236: Playback states are numbered, and say what the output was told

Status: Accepted (2026-09-26)

Refines "clients reconcile on state" in ADR-0222.

## Context

A client holds the engine's playback state from two sources: `playback.changed`
events, and the state document that answers every playback command (adopted at
once, so a button does not wait for the event). They travel differently -- in
Trackknife on different threads, on the phone as events, answers and polls --
and ADR-0222 already says events are not ordered against responses. A state
made earlier could be adopted after a later one, and whatever it held went
back for a moment.

Separately, an output applies a command on its own thread -- the local audio
worker, or an agent over the network -- so the state answering
`playback.set_volume` or `playback.seek` still carried the old volume or
place. A client that had moved its slider saw it put back: a mute undone, a
seek jumping back, until the next event.

Both showed as a window test failing about half the time under load, and as
the same flicker in use.

## Decision

- **Every playback state carries `sequence`**, a number the engine takes under
  the player's lock as it makes the state: a later state has a higher number.
  It counts per engine process. A client keeps the highest it has adopted,
  drops any state numbered at or below it, and starts again from zero on each
  connection (an engine started again counts from one). A state without the
  field -- an older engine -- is always taken. The engine's watcher leaves it
  out when deciding whether anything changed, as it does the position.
- **What the output was told is reported until it shows it.** After a volume
  or a seek, the state says the volume or place asked for until the output's
  own report matches (for a seek: at the place, or played on from it no
  further than the time since), the output changes, or two seconds pass.
- With both, a client's own commands in flight (`settling`) mean every state
  it holds predates them, so it may keep its controls as the user left them
  until they are answered.

## Consequences

- Controls do not jump back after the user moves them, in Trackknife or on
  the phone.
- `tests/engine_player_test` (a lagging output), `engine_playback_test` and
  the phone's `EngineClientTest` (an older state arriving later) hold it.

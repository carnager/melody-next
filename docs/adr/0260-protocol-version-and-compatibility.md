# ADR-0260: Protocol versions, and what 1.x keeps compatible

Status: Accepted (2026-10-03)

Answers two of the 1.0 criteria in `MILESTONES.md`: window and engine check
each other's version, and a written compatibility policy.

## Context

Trackknife, `melody-cli` and the Android client talk to engines of other
releases all the time: gemenon runs the packaged `melodyd` while a desktop
runs a newer one, the phone is updated on its own schedule. Today
`engine.info` answers `"protocol": 1` and nothing reads it. The protocol has
grown a great deal under that one number -- every ADR since 0220 added
methods -- and a client finds out what an engine lacks only when a call
fails: "this engine is too old for dynamic playlist rules" is said by the
one feature that thought to say it; others fail in their own words, or not
visibly.

The database is the other half. An engine migrates its schema forward on
start; an older one refuses a newer schema, plainly. Every migration has a
down file, applied in tests, but nothing ships that applies it.

## Decision

**Two numbers.** `engine.info` answers `protocol` and `level`:

- `protocol` changes only when a release breaks clients: a method removed,
  a parameter or answer field changing meaning. 1.x is protocol 1 throughout.
- `level` goes up with every release that adds to the protocol: a method, an
  optional parameter, a field in an answer or event. It never goes down
  within a protocol. Level 1 is the protocol as of this ADR (through
  ADR-0259). An engine that does not answer `level` is level 0.

`engine.info` also answers `release`, the engine's version, for people
reading logs and bug reports. A client never decides anything from it.

**The client decides, once, on connecting.** It reads `engine.info` and:

- **A different `protocol`:** does not use the connection, and says so
  plainly, naming which side to update: "gemenon speaks protocol 2; this
  Trackknife speaks protocol 1. Update Trackknife." The connection is not
  retried in a loop; it is tried again when asked.
- **A lower `level`** than the client knows: connects, and says once that
  the engine is older and that some things will not work until it is
  updated. What needs a newer level says so when used, as now.
- **A higher `level`:** nothing to say; an engine keeps answering older
  clients of its protocol.

The engine does not refuse clients by version: it cannot know what a client
needs, and an older client of the same protocol is always served.

**What 1.x keeps compatible.**

- **Protocol.** Within 1.x only additions: new methods, new optional
  parameters, new fields in answers and events. Clients ignore what they do
  not know. Anything else waits for protocol 2 and a 2.0.
- **Skew.** Any 1.x client works with any 1.x engine: with an older engine,
  without what it lacks; with a newer one, fully. `melody-agent` and an
  engine are the same protocol and follow the same rule.
- **Database.** An engine migrates forward on start, in one transaction.
  Every migration has a down migration, applied in the test suite; within
  1.x migrations add tables and columns rather than reshape them, so that a
  down migration loses only what the newer release added. An older engine
  refuses a newer database and says so. Downgrading is applying the down
  files in order (`docs/melody.md`), after a backup; shipping a command for
  it is a **Proposal**.
- **The window's own files** -- settings, the tab cache (ADR-0259) -- carry
  their own versions; a version a release does not know is set aside, never
  misread.

**Every change to the protocol bumps the level** in the same commit, and the
level's additions are listed in `docs/protocol-levels.md`.

## Consequences

- An engine and a client too far apart say so in one sentence, on
  connecting, instead of failing feature by feature.
- Adding a method costs a line in the level list; removing one costs a major
  release.
- The quiet period of the 1.0 criteria is measured from the last level bump.

## Verification

- `engine.info` answers `protocol`, `level` and `release`.
- The decision is a pure function, tested for each case: same, older,
  newer, other protocol, no level at all.
- Trackknife, `melody-cli` and the Android client refuse another protocol
  with that message and warn once about an older level.

# ADR-0269: Up Next is the engine's, and the window shows it

## Status

Accepted, 2026-10-05. Completes ADR-0196's "held by the engine that plays"
for the window.

## Context

The engine holds Up Next and plays it, and any client may ask for more:
`melody-cli queue`, the rofi script built on it, the phone. But Trackknife
kept a list of its own -- saved in its settings, restored at its start --
and stated it whole to the engine whenever it changed or an engine was
reached, an empty one included. The engine said only how many requests it
held. So what another client asked for played, but never showed in the
window; and a window started, or reconnected, after it could state its own
list over it, and the request was gone.

## Decision

- **The engine says when Up Next moves:** playback states carry
  `requests_revision`, bumped whenever its requests change -- added,
  reordered, played, cleared, restored at its start -- by any client.
  `playback.requests {details: true}` answers the requests as the entries
  they name, so a client can show them.
- **The window shows the engine's Up Next.** When the playing engine's
  revision moves, and none of the window's own commands is still on its
  way, the window asks for the requests and takes them as its own -- rows it
  already had keep what it knows of them. Its own changes come back the same
  and change nothing; another client's show.
- **It states nothing before it has heard.** On becoming the one playing,
  or on reconnecting, an engine's Up Next is taken, not overwritten. The one
  exception is the first answer: an engine holding none while the window
  holds requests made before the answer came -- those are sent. After that,
  an empty Up Next is someone's clearing, and it is kept.

## Consequences

- It reverses the window's earlier rule that an empty Up Next of its own is
  stated over the engine's saved asks (`engine-playback`, once
  `anEmptyUpNextTakesBackTheEnginesSavedAsks`): the problem it answered -- the
  engine playing a track the panel did not show -- is answered by the panel
  showing it.

- Protocol level 5. An older engine answers identities only; the window
  then states its Up Next to it as before.
- Undo in the window does not step back across another client's change.

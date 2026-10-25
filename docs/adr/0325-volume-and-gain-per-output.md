# ADR-0325: Volume and ReplayGain per output

## Status

Accepted, 2026-10-25. Amends ADR-0138 (ReplayGain one engine setting) and
ADR-0228 (outputs).

## Context

Volume was the output's own and nobody kept it: an agent or the engine's own
audio started again at 100 %. ReplayGain was one engine setting, saved: the
phone turning it off turned it off for the desktop's speakers too, both
playing through one engine. Clients asked that each keeps its own.

A client is not where loudness lives: the phone controlling the desktop's
speakers sets those speakers' volume. What belongs together is an output --
a set of speakers, headphones, a renderer -- and how it is listened to.

## Decision

- **The engine keeps each output's volume and ReplayGain mode** as last
  set, in its state (`outputs.settings.v1`), by output id ("local",
  "agent:phone", "upnp:…").
- **An output chosen plays as it was set**: switching to it, an agent
  coming back (started again, at its own volume), and the engine starting
  give it its volume and mode back. An output never set takes the gain the
  engine plays with and keeps its own volume.
- Preamps stay one engine setting.
- **A client never pushes its own ReplayGain.** It shows the playing
  output's mode and preamps as the engine reports them, and sends them only
  when they are changed there -- the menu, Settings, or its "auto" following
  shuffle -- and only to the engine playing. Connecting, reconnecting or
  starting sends nothing: Melody's window once gave its own setting to every
  engine it reached on each connection, and so overwrote what another output
  -- set from the phone -- was set to.
- **An engine, and an output it was never told about, plays album gain**:
  no client has to push a setting for the first track to play as Melody
  does by default.

## Verification

- `engine-playback`: a window whose own setting is album, connecting to an
  engine another client set to no gain, shows no gain and leaves it so.
- `bench-main-window`: preamps changed in Settings reach the playing engine.

- `output-agent`: two agents set differently are given their own volume
  and mode back as each is chosen again, and the engine keeps them.

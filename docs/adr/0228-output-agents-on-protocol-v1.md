# ADR-0228: Output agents on protocol v1

- Status: accepted
- Date: 2026-09-23
- Implements: the unified-engine plan's Phase 4
- Replaces: Melody's MPD-port agent protocol ("v2")

## Context

An engine plays where it runs. Sound anywhere else -- a bedside box, the
desktop while the library lives on a server -- comes from an agent the engine
drives. Melody's agents did this over its MPD port:

- `agent_register` then MPD-style commands;
- a queue the agent fetched itself with `playlistinfo` and addressed by
  position, so an edit on the server raced every `play 7`;
- no authentication;
- a streaming port hard-coded in the agent (6701);
- a choice between opening files and streaming that the server never heard
  about.

The MPD protocol is being retired (ADR-0224), and these faults are in the
protocol, not in the agent code. So the agent is redesigned rather than kept
compatible.

## Decision

**An agent is an audio device, not a player.** The engine's player already
decides everything:

- the queue, order and modes;
- gapless continuations;
- consume, Up Next and ReplayGain;
- listening history, and restoring after a restart.

It needs something to play on. That has been the local audition service; it
becomes an interface, `audio::Audition`: load and play, arm a gapless next,
clear it, play, pause, stop, seek, volume, ReplayGain mode and preamps,
restore paused, and a snapshot of the audio state. The local audition service
is one implementation. An agent reached over the network is the other. The
player drives either without knowing which, so nothing the player does is
reimplemented for agents, and an agent is small.

**The agent is C++, its own binary: `melody-agent`.** It plays with the
same audition service as the engine, so every output behaves identically:

- ReplayGain overrides from sidecars and CUE sheets;
- CUE segments, subsongs and multi-stream files;
- every format the engine opens, trackers included;
- the same gapless handover and PipeWire output.

An mpv-based Go agent was the alternative, and it is easier to deploy. It
would have needed each of those decisions sent explicitly, and it still could
not have played everything the engine can.

It is a separate binary rather than `melodyd` in a mode. It links only the
audio side (decoder, audition service, PipeWire output, protocol), with no
database, catalogue or tag writer, so it is smaller and simpler to build
self-contained. A process is either a server or an output, never "which mode
is this in". It takes the server's address and token, a name and,
optionally, a music root, as flags: it does not read Melody's
`melody-agent.toml`, which names the old server's port and protocol. It takes
over the `melody-agent` name from Melody's Go agent; each machine switches
when its binary is replaced, because the two protocols are not compatible.

**The agent connects; the server drives.** The agent opens the connection, so
an agent behind NAT or on a laptop needs no inbound port. Then:

1. It authenticates like any client (ADR-0223).
2. It sends `agent.register` with `name`, a per-process `instance`, and
   whether it has the files (`files: true` when it has a music root).
3. The connection then turns round. The server sends requests (`audition.*`)
   and the agent answers them. The agent pushes `audition.changed` events
   carrying its snapshot, on every change and every 250 ms while playing.
4. It reconnects every two seconds when the connection drops. A new
   registration under the same name replaces the old one.

**Files by identity, never by position.** Each load names the file itself and
the queue entry it plays. There is no agent-side queue to fall out of step.

**Files or a stream, and the server knows which.** The server has a
`--music-root`:

- For an agent with files, a path under it is sent relative, and the agent
  joins it to its own root, as satellite mode did. A path outside it is sent
  as it is, which suits a mount at the same place on both machines.
- For an agent without files (`melody-agent --stream`), the server sends a
  stream URL it built itself: its own HTTP listener (`--http HOST:PORT`),
  `GET /stream?path=<encoded path>&token=<token>` with range requests. No port
  is hard-coded in an agent. The host is the address the agent reached the
  engine at, unless the listener is bound to one address.
- The stream token is made when the engine starts and reaches agents only
  inside the URLs the engine sends them; it is not the engine's TCP token.
  And it opens only what the player holds -- queue entries and asks -- so a
  path the player does not hold is answered 404 whatever the token.

**Outputs.** The server lists its outputs (its own audio, if it has any, and
every registered agent) through `outputs.list` and selects one with
`outputs.select`; the choice is persisted. Switching output while playing
restores the position on the new one and stops the old, the way enabling an
output did. An agent that drops while selected leaves playback paused on it
until it returns, so a reboot of the bedside box does not move the music to
the living room. Synchronised multi-room output stays out of scope, as in the
plan.

## Distribution

`melody-agent` is built self-contained for x86_64 and aarch64, so it can be
copied to a machine and run:

- FFmpeg (audio only), libopenmpt and the C++ runtime are linked in.
- It is built against an older glibc, so it runs across current
  distributions.
- It links the system's PipeWire, which cannot meaningfully be static and is
  present wherever it plays.

A script under `scripts/` produces both builds. Melody's snapclient hook is
not carried over; synchronised multi-room output is out of scope here, as in
the plan.

## Consequences

- Melody's Go agents do not connect to the new engine. The Go `melodyd` keeps
  serving them until the new agents are installed, and the two engines can run
  side by side on different ports meanwhile.
- The engine gains an HTTP listener, which the plan's byte access for clients
  (covers, copying tracks) can later share.

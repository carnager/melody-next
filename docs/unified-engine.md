# Unified engine — migration plan

Plan for [ADR-0220](adr/0220-unified-engine-and-remote-agents.md): one engine
process owning catalogue, mutation and playback; thin clients that may be
remote; audio produced by output agents.

## Why this, and why now

Every feature in the workspace is currently built twice, once per authority,
and the ADR trail from 0210 to 0214 is a record of paying that tax repeatedly.
The cause is two catalogues behind one UI, not two locations. Two other goals
fall out of fixing it: playback that survives the UI closing, and heavy file
work (scan, ReplayGain, convert) running next to the files so a NAS library
performs like a local one.

The target end state:

```
  clients                    melodyd (C++)                 agents (Go)
  ─────────                  ─────────────                 ───────────
  Trackknife  ──┐          ┌──────────────────┐         ┌── melody-agent (satellite)
  melody-tui  ──┼── proto ─┤ catalogue        ├── proto ─┼── melody-agent (streaming)
  melody-cli  ──┤          │ mutation+journals│         └── loopback agent (own output)
  Android     ──┘          │ playback state   │
  MPD clients ── mpd ── melody-mpd bridge     │
                           └──────────────────┘
```

**Names.** Melody is the backend family, Trackknife is the app: `melodyd` (the
engine), `melody-agent`, `melody-cli`, `melody-tui`, `melody-mpd` — and
**Trackknife** as the official frontend. Trackbench is retired as a product
name; remaining internal uses (`BenchMainWindow`, `bench-*` object names,
`src/bench`) are renamed opportunistically, not as a phase of their own.

## Sequencing

Phases are ordered by dependency. Each one leaves a working application; the
current setup stays usable until Phase 6.

### Phase 0 — Playback ownership out of the widget layer

**Everything else depends on this.** `src/audio` is the audio engine, but the
brain lives in Qt: `src/bench/bench_transport.cpp` (~2000 lines),
`bench_up_next.cpp`, `bench_resume.cpp`, plus album shuffle and listen
qualification. Queue ownership, advance rules, resume checkpoints and history
events cannot cross a process boundary while they are wired to widgets.

- Introduce a core-owned playback service (no Qt widgets, no `QWidget` parents)
  holding queue, current occurrence, modes, up-next, resume checkpoints and
  listen qualification. Reuse `src/audio/src/playback_order.cpp` and
  `local_audition.cpp` as-is.
- `bench_transport` and friends become thin views over it, keeping their
  existing signals so the UI is unchanged.
- Still one process. No protocol yet.

Done when: the playback service is exercised by tests without constructing
`BenchMainWindow`, and the existing local playback, resume, up-next and album
shuffle suites pass unchanged.

### Phase 1 — Engine as a library, client as a client

- Split the engine target: catalogue, mutation, journals, query, playback
  service. It already maps onto existing CMake targets (`src/persistence`,
  `src/metadata`, `src/operations`, `src/convert`, `src/loudness`,
  `src/formats`, `src/query`, `src/audio`).
- Define the client-facing API as a C++ interface first, in-process. The UI
  calls it instead of touching `LocalLibrary`, `ListRepository` and the models
  directly.
- `src/bench/local_list_model.cpp` becomes a client-side projection fed by that
  API rather than an owner of rows.

Done when: no `src/bench` translation unit includes a persistence or operations
header directly.

### Phase 2 — Protocol v1

Requirements are in ADR-0220 and are not negotiable down: request ids,
multiplexing, pushed state events, job progress streams, cancellation, binary
side-channel. Human-readable framing (JSON-RPC-shaped over a socket) so `nc`
and a shell script remain debugging tools.

- One schema artifact checked into the repo, plus a golden corpus exercised by
  both the C++ and Go implementations. Follow the existing pattern in
  `tests/query/history_corpus.hpp` and `tests/titleformat/tkfmt-corpus/`.
- Long operations (scan, ReplayGain, convert, metadata apply) are **jobs**:
  submit, stream progress, cancel, deliver a result document. They never occupy
  the control path. This is the direct lesson of ADR-0219's cover starvation.
- Transport: unix socket first, TCP in Phase 3.

Done when: the desktop UI drives a same-machine engine process over the socket,
and the engine survives the UI exiting mid-playback.

### Phase 3 — Network, auth, and client byte access

- TCP listener with authentication required on any non-loopback bind.
- Byte access for clients that cannot read the library: artwork, waveform peaks,
  audition. Prefer engine-side rendering for peaks; range requests for artwork.
  Today these read files directly from the UI thread's workers.
- Profiles gain an endpoint. A local profile spawns a loopback engine if none is
  running; a remote profile connects.

Done when: the desktop UI runs against an engine on another machine with no
filesystem access to the library, including artwork, waveforms and a tag edit.

### Phase 4 — Output agents

Melody already implements both halves of this and they are worth reusing rather
than reinventing:

- `internal/agent/agent.go` — satellite mode: `music_dir` configured, the agent
  resolves the relative path and plays locally. No audio transport, no clock
  management. Primary mode.
- The same file's HTTP streaming fallback for agents without file access.
- `melodyd/local_agent.go` — an in-process player connected over `net.Pipe()`.
  This is the model for the engine's own output: one output code path, no local
  special case.

Port the agent contract onto protocol v1. Multi-room *synchronized* output stays
out of scope.

Done when: the engine drives its own speakers and a remote agent through the
same interface, and output selection switches between them.

### Phase 5 — MPD bridge

A separate process speaking MPD to clients and protocol v1 to the engine,
reusing `melodyd/mpd.go` and `mpd_commands.go`. Keeps `melody-cli`,
`melody-tui`, `melody-rofi` and third-party clients working.

Done when: those clients control the engine unmodified.

### Phase 6 — Catalogue unification

Last, because it migrates data and retires a working system.

- Engine catalogue becomes the only index. Melody's `tracks`, `ratings`,
  `listening_events_v`, `playlists` are retired.
- **Content identity becomes an engine primitive.** Mutations that change a
  file's hash carry ratings, history and list membership to the new identity in
  the same transaction as the write and the index update. Design this before
  writing migration code; it is the part with no precedent in either codebase.
- Migrate existing Melody ratings and listening history into the engine keyed by
  content identity, with a dry-run report before anything is written.
- The authority-specific paths in `src/bench` and `src/quick`
  (`mpd_probe_controller`, `mpd_queue_model`, the duplicated list/history/
  shuffle/resume branches) collapse into one.

Done when: `supportsCommand()`-style authority gating no longer appears in
feature code, and one implementation serves local and remote collections.

### Phase 7 — Clients

`melody-cli` (due a rewrite regardless), `melody-tui` and Android move onto
protocol v1 natively and gain file operations — tagging from the terminal, a
ReplayGain scan from a phone. `melody-mpd` stays for third-party clients.

## Verification

Per phase, in addition to the existing 69 CTest suites:

- **Phase 0**: playback service tests with no `BenchMainWindow`; existing
  resume/up-next/shuffle suites unchanged.
- **Phase 2**: protocol golden corpus run by both C++ and Go; a fault-injection
  test that submits a long job and a burst of control calls concurrently and
  asserts the control calls are not delayed or dropped — the ADR-0219 scenario.
- **Phase 3**: end-to-end against a real remote engine on the LAN host, with the
  client denied filesystem access to the library, covering artwork, waveform,
  audition, a tag write and a ReplayGain scan.
- **Phase 4**: satellite and streaming agent both driven from one engine;
  output switch mid-playback.
- **Phase 6**: migration dry-run diffed against the live Melody database; a
  re-encode test proving ratings and history survive a content-identity change.

## The melodyd cutover

`melodyd` keeps its name but changes implementation, language, database and
config layout. An existing install must not be upgraded into a different
program by accident.

- The C++ engine builds under a distinct binary name (`tkengine`, or similar)
  through Phases 0–5. Both daemons can run side by side on different ports.
- It takes the `melodyd` name, `melodyd.service` and
  `~/.config/melody/melodyd.toml` only after the Phase 6 migration is proven,
  as a major version bump.
- The Go daemon's server mode is removed in the same release; the repository
  keeps `melody-agent`, `melody-mpd` and the terminal clients.

## Open decisions

**Repository layout.** `melodyd` is now a Melody-named binary built from the
C++ tree, while `melody-agent` and the clients are Go. Either the C++ tree
becomes the home of `melodyd` and the Melody repository keeps agents and
clients, or the two merge. This determines the build and CI shape and is worth
settling before Phase 2, since the protocol schema and its golden corpus have
to live somewhere both sides build against.

**Also undecided:**

- Whether saved searches, view layouts and destination profiles are engine-side
  per collection or client-side per user. Currently client-side; engine-side
  would make them follow you between machines.
- Whether the engine exposes a web UI, or that stays Melody's `web.go`.
- Whether file-mutating jobs run in a worker subprocess from the start or after
  the first fault that takes down playback.

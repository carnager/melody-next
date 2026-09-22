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

**The duplication is not symmetric.** Almost everything Melody's daemon does,
the C++ tree already does at greater depth — catalogue and scanning
(`melodyd/db.go` + `scanner.go`, ~3.5k lines) against `src/persistence`
(~11k); tags, technicals and cover art (~800) against `src/metadata` (~14k);
the filter, album-search and history-filter code (~1.1k) against `src/query`
plus `src/titleformat` (~3.2k); up-next, album shuffle and resume against
ADRs 0210–0214; `listening.go` against ADRs 0204/0207/0208; `internal/lastfm`
against `lastfm_service.cpp`. The gap runs the other way too: `src/convert`,
`src/loudness`, `src/musicbrainz` and `src/titleformat` have no Melody
counterpart at all, and Melody only *reads* ReplayGain tags
(`scanner.go:802-805`) where Trackknife computes them.

What Melody holds that the C++ tree does not is narrow and specific:

- the **MPD protocol server** (`mpd.go`, `mpd_commands.go`, ~5k lines)
- the **agent, streaming and player stack** (`internal/agent`,
  `internal/player`, `melodyd/local_agent.go`, ~2.1k lines)

Those two are the whole reason the Go tree survives this migration, and they
are exactly what Phases 4 and 5 reuse. The web UI (`melodyd/web.go` and
`melodyd/web/`) is dropped and not replaced. Lyrics (`melodyd/lrclib.go`) is
dropped with the option of a fresh implementation later; nothing in this plan
depends on it.

So the shape of the work is not *merge two implementations*. It is: port two
Go subsystems onto the new protocol, delete the rest of the daemon, and carry
two tables of user data across. The genuinely novel problem is content
identity in Phase 6, not the capability port.

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
current setup stays usable until Phase 6. Phase 5 is the exception — nothing
depends on it and it can land late, or never.

### Phase 0 — Playback ownership out of the widget layer

**Everything else depends on this.** `src/audio` is the audio engine, but the
brain lives in Qt: `src/bench/bench_transport.cpp` (~2000 lines),
`bench_up_next.cpp` (~820), `bench_resume.cpp` (~150), plus album shuffle and
listen qualification. Queue ownership, advance rules, resume checkpoints and
history events cannot cross a process boundary while they are wired to widgets.
Transport and up-next are the bulk of the work here; resume is small by
comparison.

- Introduce a core-owned playback service (no Qt widgets, no `QWidget` parents)
  holding queue, current occurrence, modes, up-next, resume checkpoints and
  listen qualification. Reuse `src/audio/src/playback_order.cpp` and
  `local_audition.cpp` as-is.
- `bench_transport` and friends become thin views over it, keeping their
  existing signals so the UI is unchanged.
- **MPRIS moves with it.** `src/bench/mpris_service.cpp` (347 lines, `QObject`
  only, no widget coupling) attaches to the playback service rather than the
  window. Cheap now, and required later: once the engine owns playback and the
  UI can close, media keys, `playerctl` and status-bar modules must keep
  working with no window open. MPRIS is the integration surface that matters on
  a current desktop, and Melody has no D-Bus code at all.
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
- **Stable queue identity**, if the MPD bridge is being kept. The bridge
  maintains MPD song IDs and `plchanges` versioning
  (`queuePosByMPDID`, `queueIDs`, `queueVersion`), which are MPD-specific and
  have no reason to exist in the engine — but protocol v1 must expose enough
  stable per-entry queue identity and a monotonic queue version for the bridge
  to derive them. Discovered from Phase 5, settled here, because a bridge
  written against a protocol that cannot express this has to change the
  protocol. Drop this requirement if Phase 5 is dropped.

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

**The agent stays Go on purpose.** A static Go binary is copied to a Pi, a NAS
or a spare box and run — no toolchain, no Qt, no cross-compilation of the C++
tree per target. That deployment property is the reason for the language split;
it is not incidental.

**Audio never lives in the UI.** The engine's own output is the loopback agent;
a desktop that wants local audio runs a `melody-agent` process beside
Trackknife, not inside it. Anything else means closing the window stops the
music, which is what Phase 2 exists to prevent. Open: whether Trackknife
*spawns and supervises* a local agent for the single-machine case, the way a
local profile spawns a loopback engine in Phase 3, or whether the agent is
always a separately managed service.

Done when: the engine drives its own speakers and a remote agent through the
same interface, and output selection switches between them.

### Phase 5 — MPD bridge *(low priority, not on the critical path)*

**Nothing depends on this phase.** It is numbered 5 for continuity, but it can
land any time after Phase 2 — including after Phase 7, or not at all. It should
not block or reorder anything else, and the plan stays coherent if it slips
indefinitely.

The priority is low because most MPD clients are gone: Cantata is archived,
GMPC, Sonata and MPDroid are dead. The survivors are TUI, CLI and web —
ncmpcpp, ncmpc, mpc, myMPD, a few Android holdouts. Meanwhile every client in
Melody's `cmd/` is first-party and moves to protocol v1 natively in Phase 7,
and MPRIS (Phase 0) covers media keys, `playerctl` and status bars. The bridge
is not useless — it is the only thing that keeps a stranger's ncmpcpp working —
but it serves a shrinking population with a first-party replacement on both
sides.

A separate process speaking MPD to clients and protocol v1 to the engine,
reusing `melodyd/mpd.go` and `mpd_commands.go`.

Reused rather than rewritten in C++ because the bridge is a translator with
almost no state of its own: of `mpd_commands.go`'s 3,679 lines, **58 touch
backend state** through 17 accessors. The other 98% is MPD wire protocol —
the dispatch table for 92 commands, argument parsing, response formatting,
`plchanges` versioning, filter expressions, and accumulated client quirks.
Folding it into C++ means re-deriving all of that to save one process, and
loses the isolation that keeps a malformed third-party request away from
playback.

Three seams, in increasing order of difficulty:

- **Commands** — swap the 17 accessors for protocol v1 calls. Parsing and
  formatting untouched. Line count overstates the ease: 16 of the 58 are
  `c.app.db`, and synchronous local queries become round-trips that may need
  caching.
- **Events** — already the right shape. `notifyHub` fans subsystem changes
  (`player`, `playlist`, `database`, `mixer`, `rating`, …) out to
  `idle`-waiting connections and buffers those arriving while a client is not
  idling. Protocol v1's pushed state events call `notify()` instead of the
  daemon doing it in-process; the fan-out and buffering are unchanged. MPD
  `idle` and ADR-0220's pushed events are the same idea.
- **Queue identity** — the hard one, and the reason Phase 2 carries a stable
  queue identity requirement.

**Lifetime.** This phase bundles two rationales with different expiry dates.
For `melody-cli`, `melody-tui` and `melody-rofi` the bridge is *transitional* —
Phase 7 moves them onto protocol v1 natively and they stop needing it. For
third-party clients it is *permanent*, and the only thing keeping them working.
Only the second rationale justifies building it, which is why it ranks below
everything else. If it is ever dropped outright, the Phase 2 queue-identity
requirement goes with it — but that should be a decision, not a drift.

Done when: those clients control the engine unmodified.

### Phase 6 — Catalogue unification

Last, because it retires a working system — but there is no data migration.
Melody's catalogue is a subset of the engine's, so nothing is reimplemented on
the C++ side, and everything in `melody.db` is derived from the files. A scan
rebuilds it.

- **Melody's database is discarded, not migrated.** The live database holds
  66,746 tracks, one rating, zero playlists and no listening-events table at
  all; the user-authored data that a scan could not recover does not exist.
  Point the engine at the library, scan, done. If that ever stops being true —
  a real rating set or accumulated history — this becomes a migration task and
  needs a dry-run report first, but it is not one today.
- Engine catalogue becomes the only index. Melody's `tracks`, `ratings`,
  `playlists` are retired, along with `db.go`, `scanner.go`, `tags.go`,
  `technicals.go`, `cover_art.go`, `filter_expr.go`, `search_albums.go`,
  `list_search.go`, `history_filter.go`, `listening.go`, `upnext.go`, the
  `album_shuffle`/`album_random` set, `playback_resume.go` and the Last.fm
  pair. The MPD and agent stacks are the survivors.
- **Content identity becomes an engine primitive.** Mutations that change a
  file's hash carry ratings, history and list membership to the new identity in
  the same transaction as the write and the index update. This is the part with
  no precedent in either codebase, and it is now the only hard problem in this
  phase. It protects the engine's *own* ratings and history going forward — it
  is not migration machinery — so it is worth doing earlier than Phase 6 if
  convert and ReplayGain write-back (ADR-0219) start moving that data around
  before then.
- The authority-specific paths collapse into one: in `src/quick`
  (`mpd_probe_controller`, `mpd_queue_model`, `mpd_browser_model`,
  `mpd_output_model`, `mpd_search_result_model`) and in `src/bench`
  (`bench_mpd.cpp`, `bench_mpd_list_tabs.cpp`, `bench_mpd_playlists.cpp`,
  `mpd_library_search_model`), along with the duplicated list/history/
  shuffle/resume branches.

Done when: `supportsCommand()`-style authority gating no longer appears in
feature code, and one implementation serves local and remote collections. The
mechanical check, mirroring Phase 1's include check:

```sh
grep -rn supportsCommand src | grep -v '^src/quick/mpd_probe_controller'
```

must return nothing. Today it returns 44 hits across eight `src/bench`
translation units (`bench_lastfm`, `bench_up_next`, `bench_track_views`,
`bench_dynamic_playlists`, `bench_list_tabs`, and the three `bench_mpd*`
files) — that list is the phase's worklist. The probe itself may survive inside
the MPD bridge; what must disappear is feature code asking it what the
authority can do.

Not all of those hits need unification work. `bench_lastfm.cpp:352` gates on
`melody_lastfm` only to decide whether to call Trackknife's own
`LastFmService` or proxy to Melody's `melodyd/lastfm.go`. `LastFmService` is
already authority-agnostic — a Last.fm client taking a state path and an
endpoint, with no notion of where the track came from — so the engine uses it
for everything and the proxy branch is simply deleted. Melody's `lastfm.go`
retires with the Go daemon's server mode. Scrobbling needs to move with the
playback service in Phase 0, since that is where listen qualification now
lives; the context-menu love/unlove path can wait for Phase 6.

### Phase 7 — Clients

`melody-cli` (due a rewrite regardless), `melody-tui` and Android move onto
protocol v1 natively and gain file operations — tagging from the terminal, a
ReplayGain scan from a phone. `melody-mpd` stays for third-party clients if
Phase 5 happens.

**`melody-cli` is the scripting story, and that is a design goal, not a side
effect.** `mpc` is the reason MPD compatibility is worth anything in a shell,
so the rewrite has to be at least as pleasant to script against: machine-
readable output as a first-class mode, meaningful exit codes, stable field
names, no parsing of human-formatted text, and composability with the usual
pipeline tools. `melody-rofi`, `melody-musiclist` and `melody-watcher` are the
first consumers and the honest test — if they are awkward to write against
protocol v1, the CLI is not done. Getting this right is what makes Phase 5
genuinely optional rather than nominally optional.

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
- **Phase 6**: a re-encode test proving ratings, history and list membership
  survive a content-identity change. No migration test — Melody's database is
  discarded and rebuilt by scanning.

## The melodyd cutover

`melodyd` keeps its name but changes implementation, language, database and
config layout. An existing install must not be upgraded into a different
program by accident.

- The C++ engine builds under a distinct binary name (`tkengine`, or similar)
  through Phases 0–5. Both daemons can run side by side on different ports.
- It takes the `melodyd` name, `melodyd.service` and
  `~/.config/melody/melodyd.toml` only after Phase 6, as a major version bump.
  The config layout changes and `melody.db` is abandoned rather than upgraded,
  so the bump is the warning: an old install must not silently become the new
  program.
- The Go daemon's server mode is removed in the same release; the repository
  keeps `melody-agent`, `melody-mpd` and the terminal clients.

## Open decisions

**Repository layout.** `melodyd` is now a Melody-named binary built from the
C++ tree, while `melody-agent` and the clients are Go. Either the C++ tree
becomes the home of `melodyd` and the Melody repository keeps agents and
clients, or the two merge. This determines the build and CI shape and is worth
settling before Phase 2, since the protocol schema and its golden corpus have
to live somewhere both sides build against.

Post-Phase-6 the Melody repository is about 7k lines of Go — the MPD bridge,
the agent stack and the terminal clients — with no catalogue, no daemon server
mode and no web UI. That is a satellite repository, not a peer backend, which
argues for merging rather than keeping two trees in sync across a protocol
boundary that changes every phase.

**Also undecided:**

- Whether saved searches, view layouts and destination profiles are engine-side
  per collection or client-side per user. Currently client-side; engine-side
  would make them follow you between machines.
- Whether lyrics come back, and on which side. `melodyd/lrclib.go` goes away
  with the daemon; a fresh implementation would be engine-side so every client
  gets it. Not scheduled.
- Whether file-mutating jobs run in a worker subprocess from the start or after
  the first fault that takes down playback.

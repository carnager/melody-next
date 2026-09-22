# ADR-0220: One engine process, thin clients, and remote output agents

Status: Proposed

## Context

Every capability in this workspace is implemented twice. The local authority
owns `local_library_*`, `local_ratings`, `local_listening_history`,
`list_documents`, tkq evaluation and PipeWire playback; the Melody authority
owns `tracks`, `ratings`, `listening_events_v`, `playlists`, filter expressions
and its own playback. The ADR trail records the cost directly: 0210 paused
resume "in both authorities", 0211 Up Next in both, 0212 then 0213 for album
shuffle, 0214 continuous album playback in both. Each feature is designed once
and built twice, and the two builds drift.

The duplication is not caused by locality. It is caused by there being two
catalogues, two identity models and two evaluators behind one UI.

Splitting the difference — a library server plus a separate file-mutation
agent — does not resolve it. Tagging changes the thing the index describes, and
ratings and listening history hang off content identity (`rating_identity`
track/album hashes) that the writer itself invalidates. The carry-over from an
old hash to a new one must be one transaction with the file write and the index
update. Across two daemons that is a hand-rolled two-phase commit; in one
process it is a transaction. The catalogue and the mutation engine therefore
belong to the same process.

## Decision

One engine owns catalogue, file mutation and playback orchestration. Clients
are thin and may be remote. Audio output is produced by agents, including on
the engine's own machine.

**Names.** The backend family keeps the Melody name: `melodyd` is the engine,
`melody-agent` the output node, `melody-cli` and `melody-tui` the terminal
clients. The official frontend is **Trackknife**. Trackbench is retired as a
product name and survives only in internal identifiers until they are renamed.

**Engine.** A headless daemon, `melodyd`, built from the existing C++ core —
`src/metadata`,
`src/operations`, `src/persistence`, `src/convert`, `src/loudness`, `src/formats`,
`src/query`, `src/audio` — plus the playback orchestration currently resident in
the widget layer. It is authoritative for the index, ratings, listening history,
lists, query evaluation, the operation and publication journals, and playback
state. It is the only writer of music files.

**Content identity is an engine primitive.** A mutation that changes a file's
content identity carries ratings, history and list membership to the new
identity in the same transaction as the write and the index update. No client
observes an intermediate state.

**Clients.** The desktop UI stops being an authority and becomes a client. A
connection profile names an engine endpoint; a local collection is an engine on
a loopback socket, spawned by the client if it is not already running. "Local
versus Melody" is replaced by "which engine".

**Agents.** Audio output is a separate contract from control. An agent receives
transport instructions and produces sound. Satellite mode — the agent resolves
the track path against its own view of the library and plays it — is the primary
mode; HTTP streaming is the fallback for agents without file access. The
engine's own output is an agent over a loopback connection, so there is one
output code path rather than a local special case.

**Protocol.** One protocol between clients and the engine, human-readable, and
implementable without a code generator in C++, Go and Kotlin. It must provide
request identifiers, multiplexing, server-pushed state events, progress streams
for long jobs, cancellation, and a binary side-channel for artwork and audio.
These are requirements, not extensions: ADR-0219's cover regression was
job-shaped work forced down a serialized control channel until it starved.

**MPD compatibility** is a bridge process outside the engine, reusing Melody's
existing MPD command implementation. It is a supported subset, not the native
surface, and may lag the protocol without blocking it.

**The `melodyd` name outlives its implementation.** Today's `melodyd` is a Go
server; tomorrow's is the C++ engine. The Go daemon's catalogue,
playback-context and statistics implementations are retired in favour of the
engine's, while its `internal/player`, `internal/agent` satellite mode and
`mpd_commands.go` are kept and become `melody-agent` and the bridge. Because
one binary name changes language, database and configuration underneath an
existing installation, the replacement ships as a major version with an
explicit cutover: the new engine builds under a distinct binary name until the
catalogue migration in Phase 6 is proven, and only then takes the `melodyd`
name, service unit and config path.

## Scope and bounds

Synchronized multi-room output is out of scope. An output is one agent playing
one stream; no common clock, no drift correction.

A client connects to one engine at a time. Profiles switch between engines.
Simultaneous multi-engine sessions — merged search, cross-engine drag, a shared
identity space — are explicitly not designed for, though engine identity is
carried in every object reference so they remain possible later.

The engine writes files over a network and therefore requires authentication on
any non-loopback listener. Unauthenticated remote access is not a supported
configuration.

Clients no longer have implicit read access to music files. Waveform rendering,
artwork preview, audition and the properties editor obtain bytes through the
protocol or are rendered engine-side.

File-mutating work runs isolated from the audio path, so a writer fault cannot
stop playback.

No music file is mutated and no database is migrated by the restructuring
itself. Catalogue unification migrates data; it is sequenced last and separately
(see [the migration plan](../unified-engine.md)).

## Consequences

This supersedes the authority split that ADR-0058 established over ADR-0025.
There is no longer a local authority and a server authority inside one
workspace; there is one engine, reachable locally or remotely. The
"MPD-client-first" direction of ADR-0009 is retained only as the compatibility
bridge.

Playback survives the UI closing, because the UI no longer owns it. ReplayGain
scanning, conversion and tagging run where the files are, so a networked library
performs the same as a local one — the original motivation.

Clients gain file operations, not only playback. Tagging from the TUI and a
ReplayGain scan from a phone become possible, which a playback-only server can
never offer.

The cost is real: playback orchestration must leave the widget layer, a protocol
and its two implementations must be built and kept in step, and Melody's server
role is retired. The work is sequenced so that the existing setup stays usable
throughout.

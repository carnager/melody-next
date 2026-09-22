# Instructions for coding agents

## Read first

Before changing code, read:

1. [ADR-0220](docs/adr/0220-unified-engine-and-remote-agents.md) and
   [`docs/unified-engine.md`](docs/unified-engine.md) — the accepted direction
   and its phases. **This supersedes the two-authority model described in older
   ADRs, `MILESTONES.md`, and `docs/architecture.md`.**
2. [ADR-0221](docs/adr/0221-entry-and-track-identity.md) if the work touches
   lists, queues, playback position, ratings or listening history.
3. `MILESTONES.md` and identify the active milestone.
4. `docs/product.md`.
5. `docs/compatibility.md`.
6. The feature-specific document linked from `docs/README.md`.
7. `docs/architecture.md`.

Do not invent missing product decisions silently. Record consequential
decisions as an ADR under `docs/adr/`.

## Product identity

**Trackknife** is the application; **Melody** is the backend family. The
accepted direction (ADR-0220) makes the Trackknife core into `melodyd`: one
engine process owning catalogue, mutation and playback, reachable over a
protocol that works remotely, with audio produced by output agents. Trackknife
becomes a client of that engine and may be closed without stopping playback.

Trackbench is retired as a product name. Remaining internal uses
(`BenchMainWindow`, `bench-*` object names, `src/bench`) are renamed
opportunistically, not as a phase of their own.

Trackknife is a spiritual successor to foobar2000, not a visual clone and not
an attempt to run foobar2000 components. Preserve the ideas that make
foobar2000 valuable: powerful metadata operations, predictable automation,
broad format support, gapless playback, ReplayGain, speed, and user ownership.
Replace dated, modal, or obscure interaction patterns with a modern and
coherent UI.

## One implementation, not two

The two-authority model — authority-bound tabs, an active-tab authority switch,
and every feature built once per authority — is being removed. It is the tax
ADR-0220 exists to end, and the ADR trail from 0210 to 0214 records paying it
repeatedly.

- **Do not add authority-gated feature code.** No new `supportsCommand()`-style
  branching, no new `*Mpd*`-suffixed parallel to a local method, no new
  duplicated list/history/shuffle/resume path. A feature is implemented once.
- **Local versus remote is a connection, not a branch.** In the end state
  there is no local-versus-remote axis, only which engine a tab's list came
  from, with the same operations on every one. The client may hold several
  connections open at once — a desktop library and a NAS engine side by side —
  so "the active authority" stops being something the workspace has. The MPD
  Queue tab is that idea's current form and is removed, not generalised.
- **Tabs are tabs.** A tab is a set of track references, an order, and a view
  layout, distinguished only by how the set was produced and whether it
  re-evaluates. Search results are not a special kind of tab. The tagger is the
  one exception and stays a separate window (ADR-0221 rationale in
  `docs/unified-engine.md`, Phase 1).
- **`quick::MpdQueueModel` and its tab type are being deleted, not
  generalised.** They exist only because Trackknife is currently an MPD client
  when talking to the Go daemon. Do not build new abstractions over them.
- **The UI asks; the core answers.** The UI tells the core what to do and the
  core owns the database, looks things up, and returns what was asked for.
  Nothing in `src/bench` or `src/uicommon` should open a `LocalLibrary` or a
  `ListRepository`, or hold a database path. `ui::ListPersistenceService` does
  both today and is being moved to the other side of that line, not copied --
  do not model new code on it.
- **Melody's daemon is not the place for new server-side features.** Its
  catalogue, scanner, tagging, query, history and Last.fm code are all retired
  in favour of the C++ engine, which already implements them at greater depth.
  Only the MPD bridge and the agent/streaming/player stack survive.

**During the transition**, existing authority code keeps working until its phase
lands. Do not break the current application to anticipate a later phase; the
rule above governs *new* code and the direction of refactors.

## Non-negotiable behavior

- The project owns the versioned `tkfmt-1` formatting-expression language
  defined in `docs/title-formatting.md` and ADR-0008. Foobar2000 and MusicBrainz
  Picard scripts are not compatibility targets.
- Formatting is deterministic and side-effect-free. It is shared by library
  trees, track/queue views, and conversion/file naming; mutation remains in
  previewed operation plans.
- Persisted language behavior may change only through an explicit dialect
  version, never by silently reinterpreting an existing expression.
- Every decodable format must be ReplayGain-scannable. Results are embedded
  when the format has a safe, interoperable mapping and otherwise stored in a
  sidecar/library record. Never imply that analysis requires writable tags.
- Destructive metadata and filesystem operations require a complete preview,
  conflict detection, explicit commit, cancellation, and a recoverable journal
  or undo story.
- Long operations must be asynchronous, cancellable, progress-reporting, and
  parallel where the underlying codec/container libraries permit it.
- Ship and preserve the polished default workspace defined in
  `docs/ui-workspace.md`; customization is layered over it, not required setup.
- Library, playlist, and queue presentations share the declarative track-view
  engine and `tkfmt-1` formatting while retaining distinct semantics.
- Treat the performance budgets and UI-thread prohibitions in
  `docs/ui-workspace.md` as acceptance criteria.
- Preserve unknown metadata and container data whenever a file is rewritten.
- The media library, playlists, playback queue, and statistics must follow a
  successful move/rename as one logical transaction.

## Engineering expectations

- Keep the core independent from the GUI toolkit. UI code consumes typed core
  services; it does not parse tags, decode audio, or mutate files itself.
- Prefer deterministic pure functions for parsing, formatting, query planning,
  path generation, and tag transformations.
- Put language tests in a dedicated repository-owned corpus. Each case records
  its dialect, input context, source, expected output, and rationale.
- Test real files for each supported container. Synthetic metadata-only tests
  do not prove safe round trips.
- Do not claim foobar2000 or Picard script compatibility. Their public
  documentation and open implementations may inform independent design, but
  Trackknife behavior is defined by its own specification and tests.
- Trackknife source is `GPL-3.0-only`. Add the matching SPDX identifier to new
  original source files and do not add dependencies without checking GPLv3
  compatibility and preserving their notices.
- Treat paths as raw OS paths internally; do not assume valid UTF-8. Presentation
  layers may use a lossless escaped representation.
- Use bounded worker pools. Do not create one thread per track.
- Make database migrations explicit, transactional, and reversible during
  development.

## Documentation discipline

Use these labels when behavior is not yet proven:

- **Compatibility requirement**: Trackknife must match the reference.
- **Trackknife decision**: desired behavior intentionally chosen for this app.
- **Proposal**: likely direction, not committed.
- **Unknown**: requires research or a product decision.

When implementing an item, update its status in `docs/feature-matrix.md` and add
tests before marking it complete.

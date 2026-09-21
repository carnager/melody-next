# Instructions for coding agents

## Read first

Before changing code, read:

1. `MILESTONES.md` and identify the active milestone.
2. `docs/product.md`.
3. `docs/compatibility.md`.
4. The feature-specific document linked from `docs/README.md`.
5. `docs/architecture.md`.

This repository begins as a specification. Do not invent missing product
decisions silently. Record consequential decisions as an ADR under `docs/adr/`.

## Product identity

Per ADR-0058 this repository builds one primary native Linux workspace:
**Trackbench** combines an MPD/Melody client and a local-file workstation in
separate authority-bound tabs. The former `trackknife` MPD executable was
retired once Trackbench reached parity (ADR-0071); all workspace work belongs
in Trackbench.

Trackbench is a spiritual successor to foobar2000, not a visual clone and not
an attempt to run foobar2000 components. Preserve the ideas that make
foobar2000 valuable: powerful metadata operations, predictable automation,
broad format support, gapless playback, ReplayGain, speed, and user ownership.
Replace dated, modal, or obscure interaction patterns with a modern and
coherent UI. The active primary tab is the authority switch: MPD Queue uses the
server session, library, transport, and outputs; Local Queue uses raw local
files, local playback, PipeWire, and preparation tools. Never mix the two row
types in one queue or expose local tagging/filesystem mutation commands in MPD
context.

## Local and Melody feature parity

- Assess every feature for both local Trackbench playback and Melody-backed
  playback. Where it makes sense in both authorities, implement and verify both;
  do not treat a local-only implementation as completion of the feature.
- Inspect Melody in `../melody` and change it when server-side support is the
  appropriate solution. Server-owned playback, library, queue, and statistics
  behavior belongs in Melody, not a client-side emulation tied to Trackbench's
  lifetime or a single connected client.
- Reuse existing Melody capabilities where they already provide the behavior.
  Keep the user-facing workflow consistent without merging local and server
  state, identities, or mutation authority.
- Advertise and capability-gate new Melody protocol features. Preserve stock
  MPD behavior and test unsupported-server handling as well as both supported
  authorities. Explicitly document genuinely authority-specific features or
  remaining parity gaps instead of silently omitting one side.

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

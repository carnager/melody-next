# ADR-0181: Persistent client-owned server list tabs

## Status

Accepted, 2026-09-19.

Superseded by ADR-0187: server lists are MPD stored playlists, played as
Melody playback contexts. Extends ADR-0058; realizes the mixed-working-memory
half of ADR-0020 and ADR-0010 that ADR-0025's process split had shelved.

## Context

Working-list tabs have been Local-authority-only, yet the persistence
schema was built for mixed lists from day one: `ListSource::mpd` items
carry a connection-profile identity, the MPD URI as an uninterpreted
blob, and metadata snapshot fields "so remote entries stay legible while
disconnected" — implemented and covered by repository tests. ADR-0020
explicitly allowed scratch and named lists to "retain both remote
references and local raw paths as working memory". What removed the
remote half was process history: ADR-0025 split the applications and the
surviving list-tab implementation was the local one; ADR-0058 re-merged
them with authority-bound tabs whose rule targets *queues* ("MPD
occurrences and local-file rows never coexist in one queue document").
The UI enforcement was two lines — the document collector hardcoded
`ListSource::local` and the restore loop dropped everything else.

The one design question that stayed real is playback ownership: a local
list row plays through the FFmpeg/PipeWire audition engine, so a list of
server tracks must answer what Play means without blurring the transport
contract ADR-0058 protects.

## Decision

A new tab kind: **server list tabs** — persistent, client-owned working
lists of MPD tracks, stored as `ListKind::mpd` documents whose items are
all `ListSource::mpd` with full metadata snapshots.

- **Creation** is menu-driven: "Copy to server list > New list… /
  <existing list>" on the live queue, committed search tabs, and stored
  playlist tabs. Drag-and-drop creation is deferred.
- **Editing is pure client memory**: reorder (in-tab drag), removal, and
  rename mutate the document and mark it dirty; no server round trips,
  no connection required. This is the deliberate contrast to stored
  playlist tabs (ADR-0129), which are server-owned, session-only, and
  edited only through server mutations.
- **Restart and offline**: tabs restore from their snapshots with no
  connection; artist/title/duration render from the persisted fields.
  Snapshot metadata remains fallback display data, never canonical
  server metadata.
- **Playback ownership stays explicit** (ADR-0058): activation and
  double-click append to the live queue like the other MPD-context
  surfaces, and the menu's primary gesture is **Replace queue and play**
  — an explicit rewrite of the server queue, never an inferred transport
  handoff. The ADR-0180 file-operation sugar (Edit tags/ReplayGain/
  Convert) and Add to playlist appear too, capability-resolved per item
  (ADR-0020).
- **Profile identity**: items are stamped with one profile id per tab,
  captured when the list is created (existing items keep their stored
  id). Tabs are profile-agnostic for display; MPD actions address the
  current connection like every other MPD surface. Per-item profile
  fidelity across multi-server copies is an accepted simplification.
- **Rendering** reuses `quick::MpdQueueModel` (which already implements
  the shared track-row role contract) with new client-mode editing
  methods that the live-queue instance never calls; server
  reconciliation stays confined to `replaceTracks`.
- **Schema**: the state database bumps to version 37 (no DDL change) so
  older builds refuse the database cleanly instead of failing on an
  unknown document kind at load time. Local list tabs still persist only
  local rows; documents route by kind at restore.

## Consequences

- "Working lists of server tracks" exist again, as ADR-0010 originally
  specified, without weakening ADR-0058: no mixed documents (a document
  is entirely local or entirely mpd), no implicit playback transfer, no
  local mutation of MPD rows.
- The open-decisions header sentence "local operations remain
  unavailable to MPD rows" continues to hold through the ADR-0180
  materialization path; list membership itself is not a local operation.
- A future "proper queue on top of the active playlist" (recorded in
  Melody's protocol roadmap, Phase 5) would give these lists a
  server-side sibling; the client-side tabs neither require nor preclude
  it.
- Multi-row drag between tabs, drops from other MPD surfaces, and
  copying a server list into a stored playlist wholesale remain open
  follow-ups.

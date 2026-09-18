# Trackknife milestones

Milestones are capability gates, not calendar promises. The order reflects the
MPD-client-first direction accepted in ADR-0009 and the unified workspace
accepted in ADR-0058: **Trackbench** contains authority-bound **MPD Queue** and
**Local Queue** tabs. The active tab switches transport, MPD/PipeWire output
selection, and server-library/local-folders navigation while local mutation
tools remain unavailable in MPD context. Both queues use the same complete,
versioned track-view implementation. The former `trackknife` executable was
retired once Trackbench reached parity (ADR-0071).

## Status

| Milestone | State | Outcome |
| --- | --- | --- |
| M0 | Complete | Toolchain, core boundaries, fixtures, and measured legacy UI spike |
| M1 | Complete | Shared `tkfmt-1` expression engine and sandbox |
| M2 | Complete | Reliable asynchronous MPD session and domain backbone |
| M3 | Complete | Polished Qt MPD workspace — the basic MPD client is finished |
| M4 | Complete | Application split and Trackbench's playback workspace |
| M5 | Complete | Fast local tag workspace and safe file operations |
| M6 | Complete | MusicBrainz identification, AcoustID, and metadata providers |
| M7 | Complete | Universal parallel ReplayGain, storage fallback, and local gain |
| M8 | Complete | Parallel converter, resampler, and organized output |
| M9 | Complete | Melody output endpoint for the MPD client |
| M10 | Complete | Hardening, native packaging, and release acceptance |

ADR-0171 closes M5 after revalidating its exit criteria; M6's complete provider
flow had already landed. ADR-0172 closes M7, ADR-0173 closes M8, ADR-0174
closes M9, and the recorded hardening pass closes M10.

The [feature matrix](docs/feature-matrix.md) is the current capability inventory;
the [roadmap](docs/roadmap.md) prioritizes open work. The dated progress entries
below retain implementation history. Older references to two executables,
missing local indexing, or pending features may be superseded by later ADRs;
they must not be read as current workspace status.

### Requested workspace addition — optional local library (ADR-0115)

M5 is complete. At the user's request, local context now offers an optional
Library source beside Folders: chosen roots, background incremental indexing,
paged artist/album browsing and album/track search, unavailable-file retention,
and manual Refresh scans (ADR-0116). ADR-0117 adds MPD-style library actions
and drag/drop into local tab contents. Migration 28 joins index metadata/path
updates to the existing list/cache transaction. ADR-0118 adds track-number
labels and background album-cover thumbnails. ADR-0119 adds local
Repeat/Random/Single/Consume and playback ReplayGain with separate local
preferences, typed decoder metadata, and worker-side PCM gain. ADR-0120 places
MPD search inside the library panel to match the local library.
Real-file and UI verification is recorded in
[`docs/local-library.md`](docs/local-library.md). This supersedes the historical
“no local index for now” scope below; autoplaylists and logical-title indexing
remain future work.

## Rules applying to every milestone

- Keep the core independent of Qt UI types.
- MPD and local authorities stay separate even though they share one shell;
  shared behavior lives in shared internal libraries, not copies.
- Add tests with behavior; protocol behavior needs stock MPD and Melody
  provenance, and format claims need real files.
- Never block the UI thread on network, disk, decode, tags, SQL, or unbounded
  formatting.
- Use bounded workers, cancellation, progress, and structured errors.
- Use MPD song IDs for live-queue mutations whenever the protocol supports
  them; positions are display data and can race with other clients.
- Keep remote URI and local raw path distinct; Trackbench never implies
  MPD membership and the MPD client never implies local file access.
- Preserve the shipped default experience and keyboard/mouse parity in both
  applications.
- Every mutation uses plan, preview, revalidation, journal, verification, and a
  truthful recovery story.
- Update this file and `docs/feature-matrix.md` when a gate changes state.
- Do not stabilize a plugin ABI before built-in boundaries have real use.

## M0 — Decisions, skeleton, and test infrastructure

### Objective

Prove the selected C++23/Qt 6 stack, performance approach, tests, and hard core
boundaries.

### Completion evidence

- Accepted ADRs cover Qt, licensing, C++/Qt boundaries, platform baseline,
  media/storage/output backends, Unicode, and the workspace.
- CMake presets, warnings, formatting, static analysis, unit tests, sanitizers,
  fuzzing, and CI exist.
- Qt-free error/result, stable ID, cancellation, progress, and revision
  primitives have tests.
- Deterministic 10k/100k/1m fixtures and measured million-row Qt models satisfy
  the initial UI budgets.
- The skeleton contains no fake playback/tag features presented as complete.

## M1 — Trackknife formatting expressions

### Objective

Provide one deterministic expression engine for display, sorting, grouping,
queue/list views, transformations, and conversion/file naming.

### Completion evidence

- ADR-0008 and `docs/title-formatting.md` define the versioned `tkfmt-1`
  language without foobar2000 or Picard compatibility claims.
- Source-preserving parser, diagnostics, immutable programs, bounded scalar and
  multi-value evaluation, dependency extraction, caching identity, batch use,
  cancellation, and concurrency tests are complete.
- Queue, tree, and conversion relative-path fixtures share the compiled program
  type.
- The native sandbox and CLI expose live evaluation and dependencies.
- Parser and evaluator each passed 100,000-run sanitized fuzz campaigns.
- The optimized scalar benchmark evaluates one million synthetic rows at 4.49
  million rows/second; evidence is under `benchmarks/results/`.

The sandbox remains available for later hands-on UX refinement, but no longer
blocks the MPD client backbone.

## M2 — MPD session and domain backbone

### Objective

Connect reliably to stock MPD and Melody without coupling protocol work to Qt
widgets, and expose typed immutable state for the future UI.

M2 closed complete; its detailed work list and exit criteria are condensed
into the completion evidence below.

### Completion evidence

- `libmpdclient >= 2.22` sits behind a Qt-free, move-only adapter with bounded
  connect/command timeouts and TCP keepalive.
- Ordered repeated/unknown tag projection, MusicBrainz identities beyond the
  baseline enum, queue occurrence IDs, stock outputs, and Melody output
  extensions have domain tests.
- Lexical local-root mapping rejects URLs, absolute paths, traversal, empty
  components, invalid protocol UTF-8, and percent-decoding guesses.
- A scriptable fake socket-level MPD server covers greeting/version,
  capabilities/tag types, current song, queue, outputs, ping, and idle events;
  slow/disconnect/ambiguous-mutation and concurrent-client fixtures cover the
  fault surface.
- The persistent session owns separate serialized command and cancellable
  `idle` connections, publishes generation-tagged snapshots, selectively
  refreshes queue/player/output state, and reconnects with bounded backoff.
  A dropped response from a non-idempotent queue add produces an error, is
  never replayed, and is followed by a generation-tagged reconnect.
- Live-queue mutation uses stable MPD song IDs exactly once: play, append,
  insert, delete, move, crop, clear, drag-reorder permutations, and advertised
  priority control, with `plchanges` reconstruction and full-snapshot
  fallback. Stale-ID rejections classify as queue conflicts, refresh state,
  and surface a non-modal toast.
- Typed transport, volume, playback modes, ReplayGain status/control,
  additive MPD outputs, and Melody exclusive-output switching cross the
  session and controller boundaries; a live Melody system verified them by
  hand alongside the headless `trackknife-mpd-probe`.
- Typed non-recursive browse, bounded/paged any-tag search with cancellation
  and stale-result protection, stored-playlist discovery, and the complete
  advertised stored-playlist lifecycle run on the session worker.

## M3 — Polished Qt MPD workspace and queue tabs

### Objective

Deliver a genuinely pleasant daily MPD client before expanding local tooling.

Implementation note: the M3 implementation and live acceptance pass are
complete against Melody protocol 0.23.5 and stock MPD 0.24.14.
Versioned SQLite state preserves multiple connection profiles, scratch/saved
identity, tab ordering, duplicates, remote profile identity, raw local path
bytes, repeated snapshot fields, dirty/pinned state, and per-list view presets.
The workspace supports named and scratch list item editing, pin/duplicate,
keyboard tab switching/reordering, command-driven and cross-tab drag copy/move,
server-side artist/album/genre/stored-playlist browsing, continued search pages,
bounded async artwork, command discovery, and persisted configurable shortcuts.
SQLite profile/list/view state is loaded and saved on one serialized worker;
rapid edits are debounced and shutdown performs a durable worker-thread flush.

### M3 completion status (2026-08-26)

Implementation, automated verification, and live compatibility validation are
finished.

Completed evidence:

- [x] All 19 tests pass in development, ASan/UBSan, and optimized release
  builds.
- [x] Clang static analysis, warnings-as-errors, formatting, and SPDX gates
  pass.
- [x] The one-million-row UI benchmark meets every provisional p95 budget; see
  `benchmarks/results/2026-08-26-m3-offscreen-smoke.md`.
- [x] A read-only live Melody probe against `gemenon:6600` projected protocol
  0.23.5, 92 commands, 7 tag types, 18 queue occurrences, 4 outputs, and the
  advertised exclusive-output extension.
- [x] The final audit moved SQLite profile/list/view work off the UI thread,
  added debounced/durable persistence, exposed the Playlists library category,
  fixed album/artist/genre indexes being replaced by the folder model, added
  direct Enter activation, and implemented bounded exponential reconnect
  backoff with jitter.
- [x] The hands-on Melody UI/mutation/concurrent-client pass succeeded against
  protocol 0.23.5, including exclusive output switching and exact restoration
  of the shared server's original state.
- [x] The same pass succeeded against stock MPD 0.24.14, including additive
  outputs and the complete advertised stored-playlist lifecycle.
- [x] A live daemon-restart defect found during the stock pass now has a fix
  and a socket-level autonomous-reconnect regression test. Details are in
  `docs/m3-validation.md`.

### Exit criteria (met)

- An untouched first run can connect and reach a complete listening workspace
  without constructing panels.
- Browse/search, transport, output switching, live queue editing, and stored
  playlists work on both stock MPD and Melody.
- Scratch tabs survive restart with order, duplicates, remote identity, and
  dirty state intact.
- A user can complete all common queue and tab workflows with only keyboard or
  only mouse.
- Another client can edit playback/queue/output state without stale UI or lost
  local tab data.
- Measured tab switching, queue acknowledgement, scrolling, search, and artwork
  behavior meet `docs/ui-workspace.md` budgets.

With ADR-0025, M3's outcome is the finished basic MPD client. Its remaining
roadmap items are context-menu parity (delivered in M4 groundwork below), the
Melody endpoint (M9), and hardening (M10).

## M4 — Application split and Trackbench's playback workspace

### Objective

Stand up Trackbench as its own application with the migrated, proven
FFmpeg/PipeWire engine; return the MPD client to a pure server client; and
deliver Trackbench's foobar2000-inspired tabbed playback workspace with
album grouping. Performance and user experience are the primary gates. No
local library database is created; the tool grows from direct filesystem
navigation (revisit an index later if real use demands it).

### Engine and UI groundwork completed before the split

This work was done under the pre-ADR-0025 single-application plan. The engine
contracts move into Trackbench unchanged; the in-client UI surfaces are
migration sources.

- The M3 context-menu carryover reuses the registered command actions for
  live-queue, working-list, stored-playlist, search, library, and tab
  surfaces. Right-click establishes the target row/tab while preserving an
  existing multi-selection; play/open/replace, add-next/append, remove/crop,
  queue priority, copy/move, rename/pin/duplicate/reorder/close, shortcuts,
  enabled state, capability gates, and command-palette discovery share one
  action path. (Stays in the MPD client; the pattern is reused locally.)
- ADR-0014 defines one raw-path-safe ingestion path for dialogs, drag/drop,
  recent locations, and command-line arguments without creating a local
  library index. Cancellable bounded discovery recursively expands folders in
  deterministic raw-byte order, preserves duplicate occurrences, reports
  per-path failures, and persists exact local source bytes, with invalid
  UTF-8 filename coverage.
- ADR-0015 and the Qt-free `formats` adapter establish cancellable FFmpeg
  probe and streaming decode at source rate/layout: sample-domain seeking
  with flush and precise discard, end-exclusive range trim without unbounded
  buffering, and exactly-once codec/container delay and padding handling.
  Repository-owned PCM WAV, AAC-in-M4A, LAME MP3, Opus-in-Ogg, and Ogg Vorbis
  fixtures prove complete-decode identity, non-silent edges, gapless adjacent
  ranges, and sought-slice byte identity, including positive and overlapping
  best-effort timestamps.
- ADR-0016 adds the bounded local playback core: a single non-real-time
  producer fills an explicitly sized SPSC PCM ring; the real-time render
  boundary only copies/zero-fills preallocated storage and updates atomics.
  Tests prove transport states, exact pause/resume/seek, starvation recovery
  and underrun accounting, segment drain, restart, cancellation, and
  invalid-command preservation against real PCM.
- ADR-0018 adds the direct PipeWire adapter: exact source-rate interleaved
  float mono/stereo, inactive connect, mapped buffers, the requested quantum
  in an RT-safe process callback, target-object/exclusive configuration, and
  synchronous activate/drain/quiesce, with a silent live-server test.
- ADR-0021 serializes source opening, production, activation, drain, seek,
  replacement, and cancellation on one dedicated audition worker with bounded
  commands and non-blocking status publication.
- ADR-0023's list-progression semantics (previous/next through the
  originating list's local rows, single end-of-track auto-advance, visible
  failure instead of skipping) and ADR-0024's volume/device contract (cubic
  percent onto PipeWire's stream mixer, bounded sink enumeration, in-place
  retargeting preserving position/state, sticky failure visibility) are
  tested and carry into the local player. The domain chip and bound
  dual-domain transport they fed (ADR-0022) are superseded by the split.
- ADR-0017's Local Folders library tab (persisted raw-byte roots, lazy
  off-thread enumeration, symlink policy, lossless display escaping) and the
  ADR-0019 configurable lazy MPD library tree with its strict filter are
  done; the former migrates to Trackbench, the latter stays in the MPD
  client.
- Live-queue priority badges, the drag-reorder drop-indicator fix with a
  live-confirmed stable-ID permutation, and the anchored non-modal search
  panel (ADR-0013) are finished MPD-client polish.

### Work

1. Restructure the build into shared internal libraries (`core`, `titleformat`,
   `formats`, `audio`, PipeWire adapter, persistence infrastructure, reusable
   Widgets components) plus two application targets, with tests and quality
   gates covering both.
2. Create the `trackbench` executable: main
   window, tabbed track workspace, transport row, command palette, configurable
   shortcuts, toasts, and transactional persistence, reusing the shared
   components so both applications keep one visual and interaction language.
3. Remove the Local domain from the MPD client: the Local Folders tab, local
   ingestion, the audition service wiring, the domain chip, the bound
   dual-domain transport, and local volume/device controls. Provide a
   versioned SQLite migration for persisted lists containing local rows. The
   MPD client's transport controls MPD alone again.
4. Migrate ingestion (ADR-0014), folder navigation (ADR-0017), the audition
   worker (ADR-0021), progression (ADR-0023), and volume/device selection
   (ADR-0024) into Trackbench, promoting audition to first-class playback:
   play/pause/seek/previous/next, per-list progression, and end-of-track
   advance owned entirely by Trackbench.
5. Album-grouped presentation for local lists: deterministic MusicBrainz-aware
   grouping with tag fallbacks, dense grouped rows, and bounded serial cover
   loading from embedded art and folder images.
6. Local-path containment and symlink/bind-mount revalidation policy for the
   sources file operations will later accept.
7. Expand from the proven decoder-core matrix to the shipped local playback
   format matrix (at least FLAC and WavPack candidates need real fixtures),
   plus PipeWire buffer presets and hotplug/default-change reaction.
8. Gapless track transitions in the local player across the proven fixture
   matrix.

### Current progress (2026-08-29)

- Done (work items 1–4, first slice): the build now produces two executables
  over shared libraries. `src/uicommon` holds the MPD-free shared Widgets
  pieces (command palette, list persistence service, local folder tree
  model); `src/bench` builds the `trackbench` executable. Trackbench v1 has
  tabbed persistent local lists in its own SQLite database, a Folders panel
  with persisted raw-byte roots, file/folder/drag-drop/CLI ingestion
  through the ADR-0014 discovery path, and a single local transport over the
  ADR-0021 worker with ADR-0023 end-of-track auto-advance and prev/next plus
  ADR-0024 volume and PipeWire device selection.
- Done: the Local domain is removed from the MPD client — audition service
  and timer, domain chip and bound dual-domain transport, Local Folders tab,
  local ingestion/recent locations, local URL drops, and the local device
  chooser are gone; the transport controls MPD alone. Persisted local rows
  remain visible inert working memory (removable/transferable, not
  playable). The superseded chip/audition tests were deleted; all 25 tests,
  warnings-as-errors, format, and SPDX gates pass, and `ldd` shows the
  client links no PipeWire/FFmpeg while `trackbench` does.
- Done: the `formats` probe projects ordered container and best-stream tags
  (`MediaProbe::tags`, demuxer order, repeated names preserved), and a
  repository-owned tagged FLAC fixture with recorded provenance proves tag
  survival plus a 4,410-frame contiguous non-silent complete decode.
  Trackbench lists now show Title/Artist/Album/Length columns: new rows are
  enriched by a bounded cancellable background probe batch off the UI
  thread, results persist into the list document's typed fields and
  duration, and restored lists render without reprobing. CLI/startup opens
  queue until the asynchronous list restore finishes instead of being
  dropped. An offscreen end-to-end run verified the FLAC's tags and
  duration surviving into the SQLite document.
- Done: `core::revalidate_contained_source` performs the filesystem
  complement to the lexical mapping guards: it resolves the configured root
  and the referenced path to their final targets and accepts only a regular
  file strictly inside the resolved root. Tests cover in-root symlinks,
  escaping symlinks, dot-dot traversal, byte-prefix sibling directories,
  missing paths, directories, and empty inputs. File-operation call sites
  arrive with the M5 planner, which revalidates immediately before offering
  or executing every operation.
- Done: a tagged WavPack fixture with recorded provenance joins the matrix,
  proving APEv2 tag projection and a 4,410-frame contiguous non-silent
  complete decode alongside the FLAC entry.
- Done: ASan stress-running the session suite exposed two real M2-era
  concurrency bugs. First, the authoritative reconnect snapshot was
  published to observers before command acceptance was re-enabled, so an
  observer reacting to the snapshot could have a command rejected on a
  healthy connection; acceptance now precedes publication. Second, the
  command worker and the idle worker's reconnect backoff share one
  condition variable, and single-waiter notification could be consumed by
  the backoff waiter, leaving an accepted user command queued but
  unexecuted until an unrelated wakeup; enqueue/request now notify every
  waiter. The session test went from roughly one failure in twenty ASan
  runs to 0 in 150, and it now prints its wait-state flags on timeout.
- Done: Trackbench now renders the client's album-grouped presentation
  through shared code. The `QueueItemDelegate`/`QueueTableView` pair moved
  into `uicommon` behind a documented role/column contract
  (`track_row_roles.hpp`); the MPD queue model's roles alias the shared
  values so the client is behaviorally unchanged, and Trackbench's local
  list model implements the same contract. Local lists get bold
  artist—album—(date) headers with summed durations, dense numbered rows,
  the playing-row highlight, multi-selection drag reorder within a list,
  cross-tab drag copy/move, album-header click selection, and file drops
  at a precise insertion row. The probe pipeline now also captures album
  artist, date, and track number, persisted alongside the existing
  fields. A `--screenshot` QA flag (test-mode settings) renders the
  workspace headlessly; an offscreen render of a two-album fixture set
  verified header grouping, totals, and numbering. A later regression proved
  that already-enriched CUE batches use this same view path: their bounded
  first-row section resize now runs after Qt finalizes inserted header
  sections, so the album header cannot cover the first logical track.
- Done: album covers render in Trackbench group headers. The Qt-free
  `formats::load_embedded_artwork` surfaces the container's attached
  picture bytes (fixture-proven, including typed not-found and
  cancellation), and a bounded serial loader falls back to conventional
  folder images (cover/folder/front.jpg|jpeg|png), decodes and scales off
  the UI thread, caches per album group (negative results included), and
  applies images across tabs. Offscreen renders verified an embedded PNG
  and a folder cover in the same list.
- Done: folder expansion now ingests only plausible audio. Core discovery
  gained an ASCII case-insensitive extension allowlist that applies to
  directory expansion while explicitly listed files always pass; tests
  cover both sides, and Trackbench passes its audio extension list, so a
  `cover.png` in an album folder no longer becomes a track row.
- Done: natural end-of-track auto-advance — never machine-verified in the
  single-app era — is now covered by an offscreen Trackbench test playing
  a three-track list through live PipeWire (skips without a server). It
  exposed and fixed two real engine bugs: the playback worker's produce
  loop returned early when the real-time renderer finished the stream
  between producer ticks, so the terminal snapshot was never published
  and the output never drained (the UI froze on a stale "draining"); and
  the core's end-of-source state loop could regress the renderer's
  "ended" back to "draining" by reusing a stale ring measurement. The UI
  guard now also stays latched from dispatch until the player leaves
  "ended", so an advance fires exactly once per finished track, and the
  window exposes player state/position/buffer/callback properties for
  offscreen diagnostics. A tab-ownership leak found by ASan in the same
  run is fixed.
- Done: track transitions are gapless. `LocalPlayback::queue_next` chains a
  second decoder into the same ring the moment the active decode ends —
  exact-format continuations only, positions continuing monotonically in a
  produced domain past a recorded boundary with a real-time-safe crossing
  latch. A core test proves the consumed stream is the byte-exact
  concatenation of two real sources with no gap, duplicate, or early
  crossing, plus format-mismatch rejection, single-queue enforcement, and
  seek dropping the queue. The audition service exposes
  queue/clear/transition-count, rebases per-track position/duration on
  consumed takeovers, and keeps rejected continuations silent so the
  existing drain path remains the fallback for format changes. Trackbench
  keeps the engine's queued continuation in sync with the next list row
  (throttled re-requests after seeks) and moves anchors/highlight on
  transitions without issuing a load. The live-PipeWire UI test now also
  asserts the player never reports "ended" between tracks.
- Done: Trackbench's local tabs now complete the remaining lifecycle parity:
  Workspace-menu shortcuts and a tab context menu expose rename, pin/unpin,
  duplicate, explicit save, and close. Pinned tabs hide their close affordance
  and reject close commands; duplicates preserve rows under a new identity;
  the persisted modified marker and tooltip clear only on explicit save and
  return on the next edit; dirty close requires an explicit discard decision.
  An offscreen regression covers the full edit/save/dirty/pin/duplicate/close
  path and restart persistence.
- Done: ADR-0026 establishes Trackbench's Columns-UI-inspired, but independently
  specified, versioned panel-composition tree. The first renderer places the
  real Folders and Track Lists panels in the polished side-by-side default and
  lets layout-edit mode switch them to top/bottom or a tab stack and swap their
  order. Split sizes, tab selection, orientation, and order persist; strict
  bounded validation rejects malformed/duplicate/incomplete trees, while a
  newer schema falls back without overwriting the saved value. Dedicated model
  tests plus an offscreen arrange/restart/reset/future-schema pass cover it;
  later milestones add real panels rather than placeholders.
- Done: ADR-0027 extends customization into the Track Lists content rather
  than stopping at outer panes. Trackbench's default now matches the grouped
  playlist geometry: one full-width album label/duration, one large side cover
  through the visible member rows, and independent Artwork, Artist, track
  number, Title, Album, Date, and Length columns. Workspace/header actions
  switch between side-art, header-art, plain-column, and compact-queue presets;
  column order/visibility/width persist per list and can be reset or copied to
  all tabs. Visible columns auto-fill the viewport with proportional metadata
  columns, and every row/header stays single-line with right elision. Strict
  versioned validation and resize/restart/future-state tests protect the
  definition. `tkfmt-1` custom display/sort/group expressions remain the next
  view-definition layer.
- Done: a tagged AIFF fixture adds big-endian 24-bit PCM and ID3v2.4 metadata
  to the shipped playback matrix. Probe coverage requires all five tags and
  the exact container/stream shape; complete decode proves 4,410 contiguous
  non-silent frames, and a bounded seek is byte-for-byte identical to the
  corresponding complete-decode slice.
- Done: a forced-small RF64 fixture exercises the real `RF64`/`ds64` path
  without a multi-gigabyte test asset. It proves BWF/INFO metadata projection,
  48 kHz stereo 24-bit PCM shape, exactly 2,400 contiguous decoded frames, and
  a sample-bounded seek identical to the complete-decode slice.
- Done: a Sony Wave64 fixture completes the initial WAV-family container edge
  with 32-bit float stereo PCM. Tests verify the RIFF/WAVE GUIDs before probe,
  exact stream shape and frame count, and sample-bounded seek identity;
  Trackbench folder ingestion now admits `.w64` files.
- Done: ADR-0028 carries external cues from bounded parsing through Trackbench
  intake and playback. Raw `FILE` bytes and unknown directives survive parsing;
  contained sources are resolved off the UI thread and `AUDIO`/`INDEX 01`
  boundaries map from a shared 75 Hz origin into exact sample ranges. Folder
  intake creates metadata-rich logical rows without a duplicate whole-file row;
  migration 4 persists separate cue identity, physical raw path, and segment.
  The serialized player loads and gaplessly queues segments—even adjacent rows
  in the same physical file—with track-relative transport state. Parser limits,
  multi-file/data boundaries, non-UTF-8 paths, containment escape, fractional
  rates, exact PCM concatenation, UI intake, and restart restoration are tested.
- Done: ADR-0029 exposes persistent playback-buffer policy without adding
  header chrome. Responsive (250/50 ms), Balanced (750/100 ms), and Resilient
  (2,000/250 ms) profiles plus bounded exact custom values configure the
  source-rate PCM ring. Snapshots distinguish configured and active values;
  a mid-track change preserves the immutable RT ring, drops its prepared
  successor, and applies at the next ordinary load. The audio tooltip reports
  the profile, exact values, pending state, PipeWire node, and underruns.
- Done: ADR-0030 replaces menu-open enumeration with one persistent PipeWire
  registry/default-metadata monitor. Sink and default generations reach the
  playback worker within a bounded cadence. System-default streams follow the
  session manager's dynamic relinking; loss of an explicit target never falls
  back silently—it pauses at the retained sample, removes the stream, and
  reconnects paused with bounded retries when the target returns. The compact
  chooser and tooltip expose unavailable, suspended, monitor-error, recovery,
  and resolved-default state.
- Done: ADR-0031 adapts exhaustive container-native chapter tables into the
  same logical-source/sample-range model as external cues. A real Matroska/FLAC
  fixture proves two scoped-metadata chapters project to adjacent exact ranges,
  decode back to the whole PCM source, atomically replace Trackbench's
  provisional row, and survive SQLite restoration. Expansion is bounded and
  all-or-nothing, so partial, gapped, overlapping, or malformed chapter tables
  leave a fully playable whole-file row. The fixture also exposed and fixed
  coarse container-timestamp drift by counting decoded PCM contiguously after
  its seek anchor.
- Done: the Trackbench large-list benchmark exercises the real grouped local
  model/view, long metadata, cue-like shared sources, selection status, cached
  tabs, view-preset grouping, and artwork updates. It exposed Qt's
  `ResizeToContents` as an unbounded whole-model pass (roughly 760–780 ms at
  only 10,000 debug rows). Group starts now use fixed row geometry with bounded
  section overrides, an allocation-free model role, and a revision-invalidated
  geometry cache; the 100,000-row release corpus measures 3.36 ms p95 scrolling,
  6.78 ms cached tab switching, and 7.52 ms cached grouping. A diagnostic
  one-million-row run also stays inside every budget. The 10,000-row CTest smoke
  enforces them; full evidence is under `benchmarks/results/`.
- Done: Trackbench's context surfaces now reach the actual work areas rather
  than stopping at tabs and column headers. Track-row right-click preserves an
  existing multi-selection, targets a newly clicked row, or selects an album
  when invoked over its group header; it reuses Play and Remove and exposes
  selection-preserving Copy/Move submenus for every other list. Folder rows
  offer Add file/folder to current list plus directory Expand/Collapse. A
  single-track cover is right-aligned in its artwork gutter, whose first column
  is excluded from selection, playback, and focus bands.
- Done: ADR-0032 adds explicit decoder selection without conflating it with
  opaque logical identity or sample ranges. Alternate container streams remain
  opt-in because language/commentary streams are not sequential songs.
  libopenmpt's bounded identity API enumerates tracker subsong count, names,
  and musical durations while FFmpeg remains the decoder. A hand-authored
  two-subsong ProTracker fixture proves distinct selected decode, exact finite
  ranges, gapless chaining through one ring, Trackbench folder expansion, and
  SQLite restoration of stream/subsong selectors. MOD/XM/S3M/IT participate in
  ordinary folder intake.

M4 is complete. Its split, local playback, grouping, logical-track, device,
workspace, interaction, persistence, and measured-performance gates are now
covered by repository-owned tests and fixtures. Work proceeds to M5.

### Exit criteria

- Both applications build, run, and pass all tests independently; the MPD
  client links no local playback path and keeps every M3 gate green.
- A first run of Trackbench can open a folder, see album-grouped tracks,
  and play them with only keyboard or only mouse.
- Common local formats play, seek, pause, and transition gapless for proven
  codec/container fixtures, and playback cannot be starved by UI or
  background work under defined load.
- Persisted MPD-client workspaces containing local rows migrate without
  losing server-backed list content.
- Local sources are contained and revalidated before any file operation is
  offered.
- No unintended sample processing occurs with gain/DSP/conversion disabled.
- Measured list scrolling, tab switching, and grouping meet the shared UI
  budgets on large folders.

## M5 — Fast tag workspace and safe file operations

### Objective

Turn Trackbench into a serious preparation workspace where multi-file
editing feels like a modern data tool rather than a stack of per-field dialogs.

### Work

1. Local metadata/source model and read adapters for the initial real-file
   matrix, including arbitrary ordered values and MusicBrainz fields.
2. Virtualized file selector above a Fields/Original/Draft table, with
   common/mixed/missing states projected for the selected files, direct
   keyboard navigation, type-to-add/fuzzy field commands, and saved field
   layouts.
3. Staged documents, source-revision conflicts, per-format preservation tests,
   and full write previews.
4. Named transformation chains, `tkfmt-1`-derived values, numbering, and the
   separate capture-pattern language.
5. Artwork operations and clear provenance/capability display.
6. Shared rename/move/copy planner with destination preview, sanitization,
   collision checks, journaling, verification, and reconciliation.
7. Metadata-provider boundary for proposed values, identifiers, artwork
   references, provenance, and confidence, proven by internal use before any
   online provider (M6) or public plugin ABI.

### Current progress (2026-09-01)

- Done: ADR-0033 establishes the first Qt-free M5 metadata/source boundary.
  Ordered arbitrary values retain native and canonical names, qualifiers, and
  explicit cached/annotation/embedded/stream/segment/sidecar provenance;
  effective lookup has deterministic precedence, and a typed MusicBrainz
  projection covers recording, release-track, release, release-group, artist,
  album-artist, work, disc, credit, and sort-name values.
- Done: the bounded read-only TagLib property adapter brackets synchronous reads
  with raw-path source revisions (device, inode, size, nanosecond mtime), typed
  conflicts/cancellation/errors, field/value/text limits, and an inventory of
  native objects outside its text projection. A real repeated-value FLAC,
  WavPack, and invalid-UTF-8 path regression prove the boundary. The generic
  and WavPack write/preservation flags remain off; native FLAC is qualified by
  the later ADR-0043 round trip.
- Done: Trackbench's background probe now feeds the same rich document into
  ordinary files, CUE tracks, container chapters, and tracker subsongs. Lists
  cache ordered effective arbitrary/MusicBrainz fields for restart without
  treating stale source revisions as commit authority. An offscreen real-FLAC
  restart test covers the complete read, display, cache, and restore path.
- Done: ADR-0034 adds the bounded sparse multi-file selection projection and
  non-modal read-only Properties workspace. The virtual track-by-field model
  exposes common, mixed, missing, and partial states; preserves selected
  occurrences, arbitrary fields, exact ordered values, and provenance; and
  projects the selection through a concurrent boundary after opening the
  dialog shell. Edit/context-menu/`Alt+Return` entry points and real-FLAC
  offscreen coverage are in place without advertising a write capability.
- Done: ADR-0035 adds a bounded Qt-free sparse patch set with explicit ordered
  replacement versus field removal, no-op collapse, deterministic result-state
  projection, and bounded UI undo history. Properties now supports direct
  scalar keyboard editing, Delete removal, undo/redo, draft highlighting and
  counts, exact Original/Draft inspection, and explicit discard. Repeated
  values are never parsed from joined display text, and Apply remains absent.
- Done: ADR-0036 makes the compact Fields/Original/Draft table the primary
  Properties page while retaining the track-by-field matrix as a Tracks
  drill-down. Bulk scalar replacement, explicit removal, revert, and undo use
  the same bounded staged draft as individual track cells; mixed, partial, and
  missing Original states remain explicit and individual exceptions never
  masquerade as a computed aggregate result.
- Done: ADR-0037 adds the exact ordered-value editor for both bulk Fields and
  individual Tracks cells. Structured rows preserve order, duplicates,
  delimiter characters, and explicit empty values; removal remains explicit,
  and the non-blocking child editor shares the bounded draft and undo history.
- Done: ADR-0038 removes the duplicate Fields/Tracks editing modes. Properties
  now uses one vertical split: a read-only multi-select file list above the
  Field/Original/Draft table. Selecting one file exposes exact per-file values;
  selecting several produces the bulk projection and edit scope. Debounced,
  generation-safe subset summaries stay off the UI thread for nontrivial
  selections, while staged exact values survive scope changes.
- Done: ADR-0039 adds visible `Add field…`/Insert and `Remove field`/Delete
  commands to that table. Arbitrary names extend only the bounded session
  vocabulary through shared immutable item baselines; the selected-file scope
  survives insertion, and newly added fields use the ordinary draft, removal,
  revert, undo, and discard semantics.
- Done: ADR-0040 adds deterministic fuzzy completion to `Add field…` over
  present, conventional, MusicBrainz, and workspace-recent names. Canonical
  duplicates collapse, 12 ranked suggestions stay bounded, and arbitrary text
  remains valid.
- Done: ADR-0041 replaces staged-count fallbacks with a cancellable background
  projection of exact Draft result states and common values. Immutable
  copy-on-write snapshots, sparse selected-item traversal, generation checks,
  and one in-flight job keep continued editing safe and bounded.
- Done: ADR-0042 adds the complete revalidated metadata write-plan preview.
  An explicit background job rereads each staged raw source once, compares
  captured and observed revisions, retains every exact logical intent, merges
  compatible shared-source fields, and blocks changed/missing sources,
  conflicting CUE/duplicate intents, non-embedded targets, physical aliases,
  unavailable writers, and unproven unknown-data preservation. The virtualized
  preview has no Apply action.
- Done: ADR-0043 adds the first exact writable adapter for native FLAC
  Vorbis-comment text. It writes only to an exclusive prepared copy, rejects
  unrepresentable exact-empty/artwork mappings, rereads all targeted and
  untouched fields, and byte-verifies every non-comment/non-padding metadata
  block plus the compressed audio region. Real fixtures prove ordered custom
  and MusicBrainz changes while ReplayGain text, an unknown APPLICATION
  payload, embedded artwork, decoded PCM, and the original source survive.
  Compatible FLAC plans are now ready, but the UI still exposes no Apply.
- Done: ADR-0044 adds the first Qt-free commit/recovery executor and reversible
  SQLite migration 6. Native-FLAC sources serialize by physical identity and
  advisory file lock, revalidate under lock, preserve bounded Linux filesystem
  metadata, journal the exact plan and recovery paths before mutation, retain a
  byte-identical hard-link backup, atomically publish the verified sibling,
  reread the result, and require an idempotent dependent-state transaction
  before completion. Exact rollback is proven for dependent-state and injected
  journal failures; startup recovery completes an interrupted valid publication,
  removes safe pre-publication debris, and retains ambiguous evidence for
  reconciliation. Symlinks and preexisting hard links remain explicitly blocked.
  The UI still exposes no Apply.
- Done: ADR-0045 and reversible migration 7 add Trackbench's real dependent-state
  transaction. Provenance-aware item snapshots retain annotation, CUE, chapter,
  subsong, and sidecar layers while one raw-path source cache atomically refreshes
  every duplicate occurrence across every tab. Operation IDs make recovery replay
  idempotent, the cache survives restart and dominates older debounced workspace
  saves, and legacy flattened logical rows conservatively require a fresh probe.
  The serialized persistence worker and local list model expose the matching
  non-UI transaction and coherent all-row repaint boundaries.
- Done: ADR-0046 and reversible migration 8 add a separate retained-backup
  lifecycle, background startup recovery and maintenance, and the non-modal
  Metadata operations workspace. Ambiguous evidence opens automatically with
  destructive actions disabled. A verified atomic exchange provides byte-exact,
  crash-recoverable single-step undo and refreshes all persisted/visible
  occurrences with a new idempotency identity; explicit release and the fixed
  seven-day/newest-per-source/256-entry/10-GiB policy bound safe backups without
  deleting ambiguous evidence. Real-FLAC and offscreen UI tests cover the path.
- Done: ADR-0047 exposes explicit Apply only for a wholly ready immutable plan.
  A UI-captured workspace snapshot is persisted before mutation, then two
  bounded workers feed the existing per-source journaled executor. The
  window-modal job reports ordered per-source progress, structured partial
  success, and cancellation that stops new admission while in-flight sources
  reach a safe boundary. Committed results refresh all durable and visible
  occurrences and operation history; every partial/no-commit retry requires a
  fresh preview. Real-FLAC core and offscreen UI tests cover the complete path.
- Done: ADR-0048 adds the first versioned declarative metadata transformation
  chain. Ordered literal set, remove, per-value trim/lower/upper, and scalar
  `tkfmt-1` actions evaluate against the current draft on a cancellable worker;
  later actions see earlier results. Properties previews exact original/final
  cells before staging the net result through the existing sparse patch model
  as one undo transaction. Stale previews fail closed, and transformed drafts
  still require the ordinary fresh write-plan and Apply path. Qt-free and
  offscreen real-FLAC tests cover ordering, exact values, validation, limits,
  cancellation, undo/redo, and write-plan handoff.
- Done: ADR-0049 and reversible migration 9 persist up to 256 named schema-1
  transformation chains with explicit stable action codes and exact ordered
  literal payloads. Properties loads, saves, updates, saves-as-new, and deletes
  definitions through the serialized persistence worker. Schema 1 now adds
  exact append-without-dedup, whole-state copy, empty-component-preserving
  split, and single-value join semantics; Qt-free, restart, transactional, and
  offscreen save/reload tests cover the slice.
- Done: ADR-0050 and reversible migration 10 expose first-character Unicode
  capitalization as a typed, saveable transformation step. Each value keeps
  its unchanged remainder, empty values, ordering, and multi-value shape;
  core, persistence, and offscreen UI tests cover the explicit behavior.
- Done: ADR-0051 and reversible migration 11 expose all saved transformation
  chains in one persistent checkable **Tagging scripts** side panel. The
  selected row opens directly in the editor; checked chains run in displayed
  deterministic order against a temporary copy of each staged tag draft, so
  their exact results enter the final immutable write plan without polluting
  undo history or compounding across repeated previews. Apply remains explicit.
- Done: ADR-0052 promotes metadata Properties from a separately sized window to
  a temporary `Tags · N tracks` workspace tab that retains draft/apply close
  protection and stays outside persisted list documents. Transformation
  previews now lead with expandable Field/Old/New rows; file and producing-step
  diagnostics remain one disclosure level below each exact change.
- Done: ADR-0053 and reversible migration 12 add case-sensitive exact-value
  remove/replace actions plus deterministic selected-file-order numbering.
  Matching preserves every nonmatching value exactly, replacement retains an
  ordered payload, and numbering exposes bounded start/minimum-width settings;
  all three flow through saved and checked automatic chains, exact preview,
  one-step staging, and fresh physical write-plan review. Group resets and
  `TOTALTRACKS` remain explicit future numbering work.
- Done: ADR-0054 accepts one typed preparation plan for checkable tag saving,
  rename, move, and ReplayGain operations in the tagging workspace. Versioned
  output-layout profiles are independent of raw-path destination profiles and
  are shared with the future converter; combined operations prepare one final
  verified publication and enter one immutable per-file review.
- Done: ADR-0055 and reversible migration 13 persist independently versioned
  output-layout and raw-path destination profiles. The bounded Qt-free planner
  evaluates final metadata for independent rename/move choices, preserves the
  existing extension, exposes raw-to-`linux-v1` sanitization, collapses
  consistent shared logical sources, and blocks containment, collision,
  revision, device/inode alias, dependency, and path-limit conflicts against an
  explicit filesystem snapshot.
- Done: ADR-0056 and reversible migration 14 add the fresh Linux filesystem
  preflight and a distinct file-publication recovery state machine. Preflight
  opens every existing component without following symlinks, rechecks the
  exact source revision/link count, target occupancy, parent access and actual
  path limits, reports missing directories without creating them, and
  classifies atomic rename versus cross-filesystem copy. Durable states now
  distinguish prepared target, published target, dependent-state commit, and
  cross-filesystem source removal with optimistic guards and raw-path identity
  evidence.
- Done: ADR-0057 adds the first executable file-publication slice. A locked,
  descriptor-relative same-filesystem source is revalidated, planned missing
  directories are journaled before creation, and `renameat2` publishes without
  replacing an appeared target. Parent syncing, exact-identity rollback,
  idempotent dependent-state replay, startup completion, and conservative
  reconciliation pass real-file and injected-transition tests. This core is
  intentionally not exposed as a workspace action yet.
- Done: ADR-0059 and reversible migration 15 add the concrete path-aware
  dependent-state callback. One revision-guarded transaction re-keys every
  local list/logical occurrence and the source metadata cache, records exact
  idempotency evidence, and replays ordered revision-qualified relocations over
  delayed workspace snapshots without redirecting a different file that later
  reuses the path. The real same-filesystem executor now passes through this
  repository transaction, and visible models retain their current-row anchor.
- Done: ADR-0060 and reversible migration 16 make a same-filesystem undo a
  related reverse publication with its own identity. Exact locked revision and
  absent-name checks precede a journaled no-replace rename; dependent failure
  restores the forward target, rolled-back attempts can retry, completed undo
  is idempotent, and a post-callback crash reuses ordinary startup recovery.
  The real list/cache callback proves ordered A→B→A convergence.
- Done: ADR-0061 executes the cross-filesystem path. A bounded copy into the
  journal-derived target sibling preserves ownership, permissions, timestamps,
  and bounded extended attributes; syncs and byte-verifies it against the
  locked source; publishes without replacement; commits every ADR-0059 list and
  cache relocation; and only then identity-removes the original. Exact target
  rollback, cancellation cleanup, and startup recovery cover prepared,
  published, dependent-committed, and source-removed crash boundaries on real
  distinct device identities. Ambiguous evidence is retained rather than
  deleted.
- Done: ADR-0062 revision-qualifies the current decoder, prepared gapless-next
  decoder, and already queued local-audition source intents. The serialized
  publication barrier re-keys exact bindings without reopening, seeking,
  flushing, or reconnecting audio; exact replay is a no-op, stale identities
  are refused, and durable list/cache failure reverses the ephemeral player
  binding before filesystem rollback.
- Done: ADR-0063 adds the bounded file-publication Apply job. One ready review
  produces ordered no-op/commit/failure/cancellation results, serialized
  monotonic progress, fresh per-source admission, and automatic same/cross-
  filesystem dispatch on at most 1–8 workers. Shared reviewed missing-directory
  prefixes serialize only until executor-proven batch creation establishes
  them; ADR-0075 retains that evidence through a later source rollback, and
  independent file I/O then proceeds in parallel.
- Done: ADR-0064 and reversible migration 17 expose bounded Unicode-scalar
  prefix extraction as a typed saved metadata action. **Keep first characters
  of each value** defaults to 4, retains short/empty and multi-value states,
  and follows the same explicit preview, staging, and checked automatic-chain
  path as the other schema-1 actions.
- Done: ADR-0065 and reversible migration 18 add **Paste script…** as a bounded
  Picard-style cleanup translator. Supported mutations and pure expressions
  generate normal editable typed rules with source-positioned diagnostics;
  conditional removal is directly exposed and persisted with complete
  `tkfmt-1` dialect identity. The pasted source is never executed or saved.
- Done: ADR-0066 and reversible migration 19 replace separator-derived tag
  aliases with explicit adapter mappings plus independently addressable
  freeform native fields. Imported `$delete`/`$unset` rules retain exact native
  identity through preview, FLAC preparation, commit journaling, and recovery;
  a real round trip proves `ALBUM ARTIST` can be removed without touching
  conventional `ALBUMARTIST`. The TagLib-plus-FFmpeg intake merge also keeps
  FFmpeg's generic `track`, `disc`, and `album_artist` projections from becoming
  duplicate editable native fields when the authoritative embedded semantic
  property is already present; affected saved stream projections are cleaned
  on restore without removing a real embedded freeform property.
- Done: the tagging workspace now has a dedicated **Operations** side-panel
  page. **Save tags** is a real independent choice, while Rename, Move, and
  ReplayGain were initially visible but capability-gated. Reusable naming
  layouts and raw-path move destinations load, create, update, and remove
  through the serialized persistence worker without a separate settings
  window.
- Done: ADR-0067 generalizes that boundary into one immutable preparation
  review. Exact tag context, raw/sanitized targets, fresh filesystem
  classification, and blockers remain together. Qualified
  path-only Rename/Move now persists the captured workspace and reaches the
  two-worker ADR-0063 Apply surface with ordered progress, cancellation,
  partial results, visible/player reconciliation, and mandatory fresh-preview
  retry. Startup recovers both file-publication state machines and presents
  their bounded recent evidence beside metadata history; same-filesystem moves
  expose linked undo, while cross-filesystem moves remain truthfully
  non-undoable. Changed tags plus paths fail closed until a direct destination-
  artifact writer can implement ADR-0054's one-publication guarantee.
- Done: ADR-0068 and reversible migration 20 add independent `tkcapture-1`
  compilation and bounded whole-source matching with explicit unmatched,
  unique, and ambiguous outcomes. One typed saved action captures several
  fields from a filename/parent suffix, full path, current `tkfmt-1` scalar, or
  every ordered value of an existing semantic/freeform field. Later actions see
  all results; preview and staging remain atomic. Properties exposes all four
  sources and offscreen coverage saves, reloads, previews, and stages a
  multi-field filename pattern.
- Done: ADR-0069 corrects the path-only authority boundary. With **Save tags**
  off, Rename/Move materializes only the captured revision-qualified source
  tags; manual drafts stay in the workspace but are excluded, checked automatic
  chains do not run, and the Qt-free preparation planner rejects any synthetic
  metadata context. Offscreen coverage proves two fake titles cannot affect the
  reviewed or applied pathname.
- Done: ADR-0070 adds a live **Raw script** tab for the bounded cleanup subset.
  Representable typed rules round-trip to canonical editable source, valid raw
  changes immediately rebuild the typed list, diagnostics gate Preview/Save,
  unsupported typed steps fail visibly into read-only raw mode, and dirty edits
  require explicit Save or discard. Typed actions remain the persisted
  authority, so no schema migration or opaque script execution is introduced.
- Done: ADR-0072 adds the strict version-1 native tagging-script JSON envelope.
  Every current typed action, exact ordered value, match/capture mode, and full
  `tkfmt-1`/`tkcapture-1` dialect identity round-trips without exporting saved
  UUID or automatic state. Import creates a dirty unsaved definition for review;
  unknown structure fails closed, I/O is bounded and off-thread, and export is
  atomic. No persistence migration is required.
- Done: ADR-0073 and reversible migration 21 separate file topology from
  content intent and qualify the single-source destination-artifact executor.
  Changed content now follows journal-before-preparation, direct destination
  sibling creation, no-replace publication, dependent commit before exact
  source removal, conservative recovery, and explicit reconciliation on an
  unrecorded artifact. A real rich native-FLAC plan writes and rereads its
  preservation-verified result directly at a changed destination. Combined
  publication was deliberately not exposed through Properties until its batch
  and durable dependent-state composition were qualified.
- Done: ADR-0074 and reversible migration 22 compose changed native-FLAC text
  metadata plus Rename/Move through the bounded preparation Apply job. Exact
  source/revision pairing gates the plan; one transaction relocates every
  occurrence, preserves logical overlays, installs the verified target metadata
  and revision, and records idempotent refresh intent. Metadata-only members of
  a mixed batch retain journaled metadata Apply, path-only members retain the
  byte-preserving executor, and startup recovery rereads the exact published
  artifact before using the same combined callback. Trackbench reconciles the
  active player and visible rows, while changed artifacts remain explicitly
  non-undoable.
- Done: ADR-0075 corrects real destination reuse without another migration.
  A historical metadata cache with no persisted target occurrence is atomically
  superseded instead of reserving that pathname forever. The executor also
  reports exact directory-creation evidence before later work can fail, so a
  rolled-back source no longer makes its own reviewed directories look external
  to the remaining bounded batch. Active target occurrences and unexplained
  directory appearances still fail closed.
- Done: ADR-0076 adds the first Qt-free artwork-management boundary. Native
  FLAC picture blocks and exact configured sibling PNG/JPEG names become one
  bounded typed inventory with exact native type, Trackbench role, MIME,
  dimensions, size, SHA-256 identity, provenance, raw source path/revision,
  ordinal, issues, and cross-provenance duplicate linkage. The real embedded-
  PNG FLAC fixture proves inventory, external fallback, limits, cancellation,
  and unchanged typed picture evidence after an unrelated text rewrite. Native
  FLAC advertises picture reads; no artwork write or action was enabled by this
  read boundary.
- Done: ADR-0077 exposes that inventory in a lazy read-only **Artwork** section
  beneath Properties' existing file selector. One cancellable worker collapses
  repeated logical occurrences and accepts at most 64 exact physical sources;
  larger scopes ask for a narrower selection instead of truncating. Separate
  capability, inventory, and issue tables show embedded/external read support,
  revision agreement, native type, role, dimensions, size, complete SHA-256,
  raw provenance, ordinal, and duplicate linkage. No pixels, inventory rows,
  image data, or backups enter SQLite; mutation controls remained gated until
  ADR-0080.
- Done: ADR-0078 adds the first immutable native-FLAC embedded-artwork
  replace/remove plan and preservation-verified prepared-copy adapter. Repeated
  logical occurrences collapse only when ordinal, original SHA-256, intent, and
  replacement path agree; media, target, physical-alias, replacement, limit,
  and cancellation conflicts remain explicit blockers. A replacement plan
  retains only path/revision/MIME/dimensions/size/SHA evidence, never encoded
  bytes. Real multi-picture FLAC tests prove exact unrelated picture payloads,
  unchanged comments and unknown blocks, byte-identical compressed audio,
  equal decoded PCM, and source immutability for replace and remove. Native
  FLAC now advertises picture-writer adapter capability, but Properties stays
  read-only and no artwork plan, image, prepared file, or backup enters SQLite.
- Done: ADR-0079 and reversible migration 23 extend the existing metadata
  journal with explicit artwork kind, ordinal, count, target/replacement hash,
  and complete original/planned inventory digests without storing image bytes
  or inventory rows. Text and artwork now share one locked unchanged-path
  publication lifecycle. Real multi-picture tests prove atomic replace,
  retained exact-inode undo, all-occurrence revision refresh, and restart
  recovery after publication through a reopened SQLite journal.
- Done: ADR-0080 exposes native-FLAC embedded-row **Replace…** and **Remove**
  beneath the shared Properties file scope. Every action builds a cancellable
  fresh immutable review; wholly ready batches enter a two-worker job with
  ordered progress, safe cancellation, truthful partial results, mandatory
  fresh-review retry, all-occurrence refresh, and live inventory reload. The
  prepared copy now rewrites only the reviewed native FLAC picture block and
  streams every unrelated metadata/padding block and compressed-audio byte
  unchanged, avoiding collateral whole-file TagLib normalization. Qt-free
  batch tests plus a real untouched single-picture offscreen Apply prove the
  complete path without storing image payloads in SQLite.
- Done: ADR-0081 and reversible migration 24 extend the immutable artwork plan
  and journal to native-FLAC **Add…**. Fresh validation derives the append
  ordinal, rejects duplicate image bytes, and retains only role/description and
  image evidence. The direct writer inserts one picture block while preserving
  every existing picture, unrelated metadata/padding block, and compressed
  audio byte; publication, recovery, refresh, and exact Undo stay on the
  existing journaled path. **Copy to Selection** reuses Add with a revision/
  ordinal/hash-qualified embedded or external donor and creates no temp image.
  **Export…** uses a separate two-worker cancellable job with ordered partial
  results, exact-byte reread, deterministic names, and exclusive no-overwrite
  output; it creates neither SQLite evidence nor retained backups. External
  artwork remains an unmodified donor/export source, and other container
  writers remain visibly read-only.
- Done: ADR-0082 fixes repeated Rename/Move after the same physical files were
  returned to their source paths outside Trackbench. When older durable
  relocation history pre-resolves a freshly reviewed source occurrence to the
  absent target, the dependent transaction now accepts only that exact
  revision-matching target set, advances it to the verified published revision
  and metadata, and records ordinary idempotency evidence. Active target
  collisions, mismatched revisions, and missing occurrences still fail closed;
  schema 24 is unchanged.
- Done: ADR-0083 makes Apply direct. The routine review dialog and both modal
  progress dialogs are gone: a wholly ready tag/path plan enters the bounded
  Apply job immediately, progress and Stop live in the Properties footer, and
  only blocked, stopped, or failed runs open one compact feedback window
  listing the untouched files. The resizable naming-layout manager gains a
  live bounded preview table (current name → resulting path) over the
  selected tracks so the format string is trusted before applying. Planning,
  preflight, journals, recovery, and history are unchanged.
- Done: ADR-0084 removes the Preparation operations history/undo window and
  its permanently-dead Undo/Delete buttons. Crash recovery still runs at
  startup but silently; only operations recovery could neither finish nor
  safely roll back surface once, in the compact feedback window, with
  acknowledged incidents remembered. Undo backups are released at startup
  since no undo surface remains. The tag grid gains Picard-style draft
  colors on the changed content only (green added, orange changed, red
  struck-through removed) across the per-file grid and Field/Original/Draft
  rows. Also fixes a real progress race in the artwork batch executor where
  the completed count was published outside the delivery lock.
- Done: ADR-0085 turns the Artwork tab into a picture-first surface. Every
  inventory row leads with a background-decoded, revision-qualified
  thumbnail; the columns compress to File/Role/Image/Source with hashes,
  native types, and ordinals in tooltips; the storage note and the always-on
  capabilities table are gone, with genuinely view-only files reported once
  in the problems pane. Add/Replace/Remove/Copy and Export now follow the
  ADR-0083 contract: direct apply with inline progress and Stop, compact
  problems-only feedback, and no review or modal progress dialogs.
- Done: ADR-0086 lands the M5 provider boundary (work item 7). A Qt-free
  typed contract carries proposed ordered field values (identifiers travel
  as fields), artwork references, provider provenance, per-proposal
  confidence, and rationale; observation-only providers convert through one
  validated preview into an ordinary staged, colored, undoable draft
  transaction. The internal selection-consistency provider proves it end to
  end behind the Properties **Suggest** button: exact-agreement ALBUMARTIST
  fill and contiguous-run TOTALTRACKS per album group, with Qt-free and
  offscreen coverage. M6 MusicBrainz implements this same contract.
- Fixed: Suggest no longer skips files whose totals or album artist exist
  only as cached-snapshot/stream projections. Satisfied and unchanged
  checks compare against the writable (draft-or-embedded) state and see
  draft patches in both the logical and exact-native registries, so
  re-running Suggest over its own staged draft is quiet. Proven by Qt-free
  provenance regressions and a real-file pipeline reproduction. Known
  limit: a phantom value exactly equal to the proposal still collapses at
  the shared no-op staging boundary.
- Done: ADR-0087 makes TRACKTOTAL/TOTALTRACKS and DISCTOTAL/TOTALDISCS one
  Picard-style identity. Canonicalization merges the spellings, so the
  grid, drafts, naming expressions, capture, and providers see one logical
  totals field; the reader surfaces the primary spelling when a file
  carries both while preserving the secondary comment's bytes; and the
  FLAC writer erases and rewrites both paired spellings on every replace
  and removes both on removal, verified by a real-FLAC round trip and the
  mixed-album end-to-end reproduction.
- Fixed: Properties now resolves a requested freeform field against both
  logical and exact-native identities before extending its grid. Editing an
  existing native `TEST` field as `test` no longer emits a false Qt column
  insertion or invalidates the bottom-right index of a 20-file selection; an
  offscreen regression retains that exact model shape and requires zero insert
  signals.
- Fixed: Trackbench's always-live MPD queue can request covers while its tab is
  hidden. The adapter no longer treats an ordinary server-limited binary chunk
  as the end of the image; socket-level coverage assembles both `albumart` and
  `readpicture` through the explicit empty response, preventing truncated JPEG
  bytes from reaching Qt.
- Done: ADR-0095 qualifies the native WavPack text writer. The
  format-agnostic writer core (exclusive prepared copies, plan/original
  verification, PropertyMap application with the ADR-0087 paired-totals
  rules) is shared with FLAC from one internal header, and the WavPack
  prepare adds its own binary proof: WavPack blocks byte-identical up to
  the APEv2 trailer, every binary/external APE item (cover art included)
  byte-exact with none invented, ID3v1 trailers rejected as unqualified.
  Commit and refreshed publication dispatch by adapter; artwork stays
  FLAC-only. Real-fixture round-trip and blocked-trailer tests cover it.
- Done: ADR-0103 qualifies the MP3 (ID3v2) text writer as the third
  format: region-based preservation proof (audio bytes identical through
  leading-tag resizes, trailing ID3v1/APEv2 accounted, APE binary items
  byte-exact via a shared trailer detail header), paired totals disabled
  for ID3, and one adapter-keyed prepare dispatch for every qualified
  format. Musepack/Monkey's Audio wait on fixture encoders; Ogg
  Vorbis/Opus wait on a decoded-PCM verification design.
- Done: ADR-0104 closes grouped numbering: a chain action numbering
  selected items with an independent counter per evaluated tkfmt-1
  group value (per-album/per-disc), persisted as kind 17 through
  migration 26, interchanged as number_grouped_items, and offered in
  the script editor as "Number within each group".
- Done: ADR-0114 qualifies Ogg Vorbis and Opus text writers, proving
  preservation at the logical packet layer (every packet except the
  comment packet byte-identical, decode-verified in tests) with the
  shared text-writer core and paired totals enabled.
- Done: ADR-0136 qualifies the MP4/M4A text writer over the shared
  prepared-copy core: standard atoms through TagLib's documented table,
  exact `com.apple.iTunes` freeform identity for everything else, the
  combined `trkn` number/total value with paired totals off, and a
  box-level preservation proof (`ftyp`, `mdat`, and every other
  non-`moov`/`free` top-level box byte-identical) plus decoded-PCM
  equality and `covr` survival in the real-file test.
- Done: ADR-0137 extends qualified embedded artwork from native FLAC to
  MP3 (ID3v2 APIC) and MP4 (covr) across the inventory, the byte reread
  (donors, export, thumbnails, conversion carriage), the write plan, new
  prepared-copy writers reusing the containers' text-writer preservation
  proofs, and the journaled commit — whose projected inventory now
  branches per adapter (covr entries are untyped front covers, APIC
  shares FLAC's type vocabulary). The journal schema is unchanged;
  real-file tests round-trip replace/remove/add per adapter and a commit
  case proves the projection plus byte-exact undo.
- Done: ADR-0169 adds separately versioned `portable-v1` filename handling to
  saved naming layouts and their live preview without changing `linux-v1`.
- Done: ADR-0170 adds bounded saved tag-field layouts. Selected/visible fields
  become an ordered reusable preset, the active preset loads before projection,
  and hidden fields remain part of the complete draft and Apply.
- Done: ADR-0171 revalidates the M5 exit gate and bounds future format/policy
  expansion. Unsupported writers stay read-only; new adapters and typed
  actions require independent preservation or semantic proof.

### Exit criteria

- Common bulk edits need no repetitive per-target clicking or fixed giant
  dropdown navigation.
- Keyboard-only bulk editing is fast on large selections.
- Each advertised writable format passes real round-trip, unknown-data,
  artwork, MusicBrainz, and audio-essence preservation tests.
- Plans stop on source revision changes and injected failures leave a valid
  original or documented recovery journal.
- Local references in every affected tab follow successful file moves as one
  logical update.

## M6 — MusicBrainz identification and metadata providers

### Objective

Make MusicBrainz support real: identify local files and propose complete,
provenance-carrying metadata through the M5 provider boundary, as explicit
network operations that never bypass staged preview/write safety.

### Work

1. MusicBrainz web-service client with honest rate limiting, caching, offline
   behavior, and clear failure states.
2. Release/recording matching for selections and albums: search, disc-shaped
   candidate ranking, track alignment, and a side-by-side proposal review in
   the staged metadata preview.
3. Complete identifier coverage: recording, track, release, release-group,
   artist, work, and disc IDs; credited names kept separate from sort names.
4. Cover Art Archive artwork proposals through the existing artwork
   operations.
5. Optional acoustic fingerprinting (AcoustID) evaluated behind the same
   provider boundary; it ships only if match quality justifies the dependency.
6. Provider provenance and confidence surfaced in the review UI; accepted
   proposals become ordinary staged edits.

### Exit criteria

- A ripped or downloaded album with poor tags can be identified, reviewed
  field-by-field, and committed without hand-typing standard metadata.
- Every network lookup is explicit, cancellable, cached, and fails visibly
  without corrupting staged work.
- Accepted proposals round-trip through the same preservation tests as manual
  edits; rejected proposals leave no trace in files.
- MusicBrainz-aware grouping and sorting improve measurably on the fixture
  corpus without breaking deterministic fallbacks for untagged files.

### Current progress (2026-09-02)

- Done: ADR-0088 lands the ws/2 client core. Typed URL builders cover
  Lucene-escaped text search — artist and album with no MusicBrainz id
  anywhere — and release lookups with recordings, artist credits, release
  groups, and labels; pure bounded parsers keep everything an in-app
  version picker needs per candidate (score, date, country, status,
  disambiguation, barcode, label/catalog number, media formats, and the
  release-group id that groups versions), and per-medium track listings
  with track/recording ids and lengths. One serialized cache-first fetcher
  paces dispatches at 1.1 s with an identifying User-Agent and maps
  offline/throttled/missing/unexpected outcomes to typed errors. Migration
  25 adds the bounded 14-day, 10,000-entry SQLite response cache. Fixture,
  scripted-transport, and cache tests cover the whole boundary; no UI or
  matching yet.
- Done: ADR-0089 adds deterministic, network-free release matching. Ranking
  orders candidates by MusicBrainz score plus track-count corroboration
  without ever filtering versions away; alignment prefers an exact
  (disc, track-number) permutation, then plain order, then a conservative
  greedy fallback over normalized-title similarity, duration proximity, and
  position agreement that leaves unfittable files unmatched at zero
  confidence. The proposal bridge converts one aligned release into
  ADR-0086 proposals — per-track titles/artists with join-phrase credits,
  album-level fields, per-medium totals, multi-disc numbering, and the full
  MusicBrainz identifier set — each carrying confidence and a rationale
  naming the exact release version, suppressed entirely below the
  confidence floor. Only the in-app search/version-picker UI remains before
  end-to-end identification.
- Done: ADR-0090 lands that UI. Properties gains Identify…, which builds
  local descriptors from the selection's baseline tags and opens an in-app
  window-modal search dialog needing no MusicBrainz id anywhere — the
  lookup Picard delegates to a browser. Every release version is a distinct
  ranked row (score, album, credits, tracks, media, and a
  date · country · disambiguation · label · catalog version column);
  choosing one runs alignment plus the proposal bridge and stages the
  result through the ordinary ADR-0086 preview as one undoable colored
  draft transaction — nothing written until Apply. A lazily created bench
  fetch service composes the paced ADR-0088 client with worker-thread
  reads and writes of the migration-25 cache, and an injected scripted
  service keeps the whole flow offscreen-tested end-to-end. Cover Art
  Archive proposals and the AcoustID evaluation remain.
- Done: ADR-0091 lands Cover Art Archive front covers through the existing
  artwork operations. The artwork tab gains Fetch cover, enabled exactly
  when every selected file carries the same MUSICBRAINZ_ALBUMID — read
  draft-or-baseline, so a just-identified selection qualifies before
  Apply. Two paced, cached requests resolve the archive listing (bounded
  typed parsing, https upgrade, deterministic front selection) and the
  image; PNG/JPEG magic is verified and the bytes enter the ordinary
  preservation-exact prepared-copy add path as a direct journaled apply.
  The shared transport now follows cross-origin redirects while still
  refusing scheme downgrades. An injected cover service keeps the flow
  offscreen-tested end-to-end against a real FLAC. Non-front image
  choices and the AcoustID evaluation remain.
- Done: ADR-0092 completes Picard-parity proposals. The release lookup
  now includes ISRCs and recording-level work relations, and parsing
  carries release-group types and first-release dates, script/language,
  medium titles, and UUID-validated work ids. The bridge proposes the
  full matched-release tag set — sort names joined like credited names
  but never merged with them, ORIGINALDATE/ORIGINALYEAR, lowercased
  RELEASETYPE/RELEASESTATUS, RELEASECOUNTRY, SCRIPT, LANGUAGE, LABEL,
  CATALOGNUMBER, BARCODE, MEDIA, DISCSUBTITLE, multi-value ISRC, and
  MUSICBRAINZ_WORKID — with Picard's 1/1 numbering on single-disc
  releases, and six new conventional FLAC identities keep those fields
  one logical column each. Disc ids stay out of scope (they need
  physical TOC data); AcoustID remains the open evaluation.
- Fixed: exact-native mutations of a paired totals spelling now address
  the whole pair (ADR-0087 addendum). An automatic chain removing
  totaltracks in exact-native mode against Picard-tagged files no longer
  fails every apply with a reread mismatch, and the partner spelling can
  no longer resurrect a removed value.
- Done: ADR-0093 makes Apply pure WYSIWYG. Automatic scripts no longer
  run hidden inside the write plan; they stage their edits as ordinary
  colored, undoable draft transactions when the grid loads over the
  local baseline (beyond Picard, which only scripts MusicBrainz
  results), after Suggest/Identify staging, and when a script is checked
  — idempotently, off the UI thread. The write plan is built from the
  staged draft alone, so the grid is the write and Suggest stops
  refilling fields the user's own scripts delete. Script and provider
  edits render in italics with source-naming tooltips, and a sticky
  footer summary ("MPD Library staged 12 edits · Undo") offers
  one-click rejection — information instead of a confirmation gate.
- Fixed: Fetch cover now replaces a file's existing embedded front
  picture (ordinal- and fingerprint-targeted) instead of stacking a
  second front, adding one only where none exists (ADR-0091 addendum).
- Done: ADR-0094 adds the archive image picker. Covers… lists every
  Cover Art Archive image of the release — types, approval, comments,
  and 250px thumbnails trickling through the paced fetcher — and one
  choice places the image by mapped role: Front runs the replace-or-add
  front placement, Back/Medium map to back/disc, the rest to other, all
  through the ordinary preservation-exact review. The cover service is
  recomposed into listing/bytes/store functions so the section owns the
  orchestration and the scripted seam still tests everything offscreen.
  M6's remaining open item is the optional AcoustID evaluation.
- Done: ADR-0096 evaluates and ships AcoustID. Ground-truth evaluation
  against the real Picard-tagged corpus (embedded recording ids): 11/11
  studio top hits, 8/9 live, 8/8 plausible matches on a fully untagged
  album — quality justifies the dependency, and fpcalc runs as an
  external tool exactly like Picard's, no build dependency. The
  Identify dialog's Fingerprint files button fingerprints the selection,
  votes releases by matched-recording coverage at a paced 400 ms
  interval, and feeds the top candidates ("7/9 files") into the same
  version picker and staging path. The user-registered client key lives
  in QSettings and is never compiled in. With this, every planned M6
  work item is complete.

## M7 — Universal parallel ReplayGain

### Objective

Treat loudness as first-class metadata while analyzing every decodable source
quickly and correctly.

### Work

1. Validated libebur128 streaming analysis at the documented ReplayGain target.
2. Track/sample peak, optional true peak, correct album programme reducers,
   and explicit high-rate policy.
3. Track, selection-as-album, release-aware, and `tkfmt-1` grouping modes.
4. Resource-aware parallel FFmpeg decode graph with bounded codec threads.
5. Result grid integrated with metadata workflows, incomplete-album warnings,
   retry, preview, and quiet presets.
6. Safe per-format embedded mappings plus sidecar fallback.
7. Local playback gain modes, preamps, and peak-based clipping prevention.

### Exit criteria

- Every decodable fixture can be scanned regardless of tag writability.
- Album loudness is measured as one programme, never averaged from track gain.
- Results cannot be applied to a changed source revision.
- Embedded and sidecar results round-trip with provenance and interoperate with
  independent players where a standard mapping exists.
- Multicore scanning scales without starving playback, interaction, or storage.

### Current progress (2026-09-02)

- Done: ADR-0097 lands the validated measurement core. Trackknife::Loudness
  wraps libebur128 over the qualified decoder's interleaved float PCM:
  gated integrated loudness, sample peak, optional true peak, native-rate
  analysis for high-rate material, and ReplayGain 2.0 gains at the -18 LUFS
  target. Album loudness combines retained analyzer states as one gated
  programme, never an average. Validation pins the ITU-R BS.1770
  conformance points (997 Hz full-scale: -3.01 LKFS mono, ~0.00 stereo,
  exact -6.02 dB per halving), programme-over-average album behaviour,
  true peak >= sample peak, and honest unmeasurability for sub-400 ms
  material through the real decoded fixture.
- Done: ADR-0098 adds the bounded parallel scan graph: 1-16 pull-based
  workers (one decoder each, so codec threading never multiplies), cue
  segments and subsong selections through the decoder's segment opens,
  per-item revision capture with post-decode re-verification (a source
  changed mid-measurement fails typed), mutex-serialized progress, clean
  cancellation, and album reduction from retained analyzer states with
  incomplete-programme warnings instead of partial-album numbers. Tests
  pin parallel-equals-direct on generated WAV albums plus failure
  isolation and cancellation.
- Done: ADR-0102 bridges the MPD queue to the library tree: Go to
  artist / Go to album context actions asynchronously load roots, fetch
  the artist branch, and select the album with definition-aware
  matching; misses report in the status bar instead of failing silently.
- Done: ADR-0101 (with addendum) rebuilds the local Sources panel: a
  labeled bookmarks section (old library roots migrated, home seeded)
  above a real filesystem tree from / — hidden entries skipped,
  directories first, audio files only, folder/audio icons, home
  revealed at startup — with asynchronous bookmark reveal.
- Done: ADR-0100 wires the user-facing scan. ReplayGain scan replaces the
  inert checkbox with a grouping combo (release default, selection,
  track, tkfmt-1); measurement runs off the UI thread over
  draft-materialized documents and stages RG2-formatted values — C-locale
  decimal points via to_chars, never the user locale's comma — as
  colored drafts with italic ReplayGain provenance through the ordinary
  proposal pipeline and WYSIWYG apply. Footer progress with a Stop link;
  unmeasurable files and incomplete programmes go to the problems-only
  feedback dialog. REPLAYGAIN_* joined the conventional registry so
  rescans update existing tags instead of growing duplicate columns.
- Done: ADR-0099 adds the four grouping modes as one pure function from
  metadata documents to per-item album keys: track, selection-as-album,
  release-aware (MUSICBRAINZ_ALBUMID first, deterministic album+artist
  fallback, unidentifiable files stay track-only), and compiled tkfmt-1
  expressions over effective canonical values with empty results staying
  track-only. Namespaced keys never collide across modes.

### Correctness follow-up (2026-09-06)

- ADR-0124 fixes Properties discarding CUE/chapter sample ranges and tracker
  subsong selections. Real-file workspace regressions reproduced all three
  incorrect-gain cases and now compare track/album results with direct scans,
  including subset rescans after undo. The physical write planner blocks
  logical-source loudness from becoming whole-file tags. Durable logical
  storage remains open; this does not close M7 or change the active M5 gate.

### Playback preamps (2026-09-10)

- ADR-0138 delivers work item 7's preamp piece: separate ±20 dB preamps
  for tracks with and without loudness data, applied only while local
  ReplayGain is active (with-data stacks under the existing peak limit;
  without-data levels unnormalized sources), carried through the
  serialized worker command path, inherited by loaded sources, surfaced
  in the snapshot, and persisted behind a Preamp dialog in the local
  ReplayGain menu. The specification's additional processing-mode
  policies and true-peak limiting remain open.

### CUE sheet ReplayGain carriage (2026-09-10)

- ADR-0139 closes the CUE half of work item 8's durable-storage gap
  with the interoperable foobar2000 convention instead of a proprietary
  sidecar: scan results for CUE logical tracks route through the write
  plan into `REM REPLAYGAIN_*` lines (per-track in the TRACK block,
  album values in the header with cross-track agreement enforced) and
  publish via a revision-gated atomic rewrite whose reparse proof keeps
  every other byte — BOM, terminators, indentation, unknown directives.
  Local playback consumes the sheet values as an explicit override that
  outranks the physical file's whole-file tags, at load and across
  gapless takeovers. Undo-journal parity for sheet rewrites and durable
  storage for chapters/subsongs/unwritable formats remain open.

### Loudness sidecar (2026-09-10)

- ADR-0141 completes work item 8's durable-storage decision for the
  remaining logical tracks: container chapters, codec subsongs, and
  segments persist into `<file>.tkmeta` beside the source — strict
  versioned JSON with a bounded fail-closed codec, entries keyed by
  decoder selection + sample range, staleness by audio size+mtime so
  timestamp-preserving copies stay valid. The write plan routes their
  ReplayGain there; the commit merges revision-gated and atomically and
  deletes emptied sidecars. Probing projects fresh sidecars onto rows
  at sidecar provenance and playback consumes them first (sidecar →
  CUE → embedded). The explicit unwritable-format fallback and shared
  undo-journal parity remain open.

### Unwritable-format sidecar fallback (2026-09-11)

- ADR-0143 closes the last storage gap for local ReplayGain: whole-file
  tracks whose adapter cannot take a safe tag write (WAV/AIFF/APE via
  `taglib-properties-v1`) divert their clean conventional
  `REPLAYGAIN_*` changes into the file's loudness sidecar as a
  whole-file entry during the planner's reader pass — merged with any
  logical-track entries, revision-gated in the sidecar commit, visible
  through provenance and the apply summary. Other staged fields keep
  the `writer_unavailable` block, conflicting or exact-native edits do
  not divert, and writable formats keep using ordinary tags. Undo-
  journal parity remains the shared ADR-0139/0141 follow-up.

### Journal parity for carrier rewrites (2026-09-11)

- ADR-0145 retires that shared follow-up: CUE-sheet rewrites and merges
  over an existing loudness sidecar now run the complete ADR-0059
  journal lifecycle — the shared publish sequence (flock, hard-link
  backup, rename, fsync discipline), crash recovery that rolls forward
  with per-carrier content verification and rolls back prepublication
  debris, retained undoable byte pre-images (RENAME_EXCHANGE undo,
  verified against original evidence captured from the parsed
  pre-image), and reconciliation reporting. Evidence reuses the text
  change rows with the carrier identity in `exact_native_name`; schema
  version 29 widens the persisted content kinds by rebuilding the
  journal table without disturbing its children. Emptied sidecars now
  publish as empty documents instead of vanishing; sidecar creation
  stays a direct atomic publish because no pre-image is at risk.

### Result workflow (2026-09-11)

- ADR-0146 covers the workflow half of the result-review surface: a
  persisted "Store in sidecar only" policy diverts loudness on every
  non-CUE source into the sidecar even for writable formats (CUE
  tracks keep their sheet), failed or cancelled measurements re-run
  through a "Retry N failed" status link (structurally unmeasurable
  tracks excluded), and an "Album merging discs" grouping mode strips
  trailing disc designators from the album fallback key so
  "Album (Disc 1)"/"Album CD2" measure as one programme. Export and a
  loudness provenance inspection view remain open.

### Export and provenance view (2026-09-11)

- ADR-0147 completes the result-review surface: the scan worker
  retains one CSV row per measured item (title, path, LUFS, gains,
  peaks, album key, outcome state) and an "Export results" status link
  saves them atomically as UTF-8 CSV — a snapshot of the measurement,
  independent of later draft edits. A "Loudness sources…" button opens
  a read-only per-track table showing each effective `REPLAYGAIN_*`
  value with its origin (draft, sidecar, CUE segment, embedded), a
  pure projection of the staged selection and patch set with no I/O.

### M7 closure (2026-09-16)

- ADR-0172 reconciles all five exit criteria and closes M7. A corpus regression
  now scans every repository audio fixture through the real FFmpeg/libebur128
  graph, independently of tag writability. Qualified embedded mappings,
  CUE carriage, and automatic logical/unwritable sidecar fallback cover every
  source; revision gates and provenance round trips remain enforced. The
  interactive graph defaults to two of its bounded 1-16 workers and stays off
  both UI and real-time playback threads. The existing local source modes,
  preamps, exact bypass, and matching stored-peak protection complete the M7
  playback policy; additional DSP preference variants remain future work.

## M8 — Parallel converter, resampler, and organized output

### Objective

Convert whole lists or selections quickly into predictable destinations with
no partial output. The destination is plain filesystem output — it may be an
MPD music root, but local publication does not invoke MPD. A database update
afterwards is a separate explicit command in Trackbench's MPD authority.

### Work

1. Versioned FFmpeg encoder/container presets and capability probing.
2. Destination modes: mirror source-root structure, choose a useful preset, or
   evaluate a `tkfmt-1` relative path below an explicit root.
3. High-quality resampling, channel/bit-depth policy, dither, optional DSP and
   ReplayGain application, metadata/artwork mapping, and output post-scan.
4. One-output-per-track, grouped/cue-aware, and merge modes.
5. Bounded decode/encode scheduling, progress, cancellation, retry, atomic
   publication, verification, and conflict preview.
6. Copy/move/converted publication below a configured root with explicit
   destination preview, staged verification, and atomic commit. Move deletes
   originals only after verified publication.

### Progress

- Done: ADR-0105 lands the qualified conversion core: a Qt-free convert
  library where versioned presets (flac, opus-192, mp3-v0, vorbis-q6)
  with runtime encoder/muxer probing drive decoder → swresample →
  FFmpeg encode into a hidden temporary that is decoded end to end,
  duration- and format-verified, then atomically renamed with
  RENAME_NOREPLACE. Failure and cancellation leave nothing; cue-sheet
  sample ranges convert like whole files.
- Done: ADR-0106 adds metadata carriage (mux-time mapping — verbatim
  Vorbis-comment keys, ID3 generic frames plus TXXX — verified by exact
  reread before the atomic rename) and scan_conversion: a 1-16 worker
  pull pool with per-item revision capture/recheck, isolated typed
  failures, atomic in-scan collision resolution, throttled per-item
  progress, and clean cancellation.
- Done: ADR-0107 lands converted-publication planning and the converter
  dialog: the output-path planner gains a ConvertedPublicationPolicy
  (target extension in names and %extension%, cue subtracks fan out to
  one target per logical item instead of deduplicating per source), and
  "Convert files…" offers presets with probed availability, a
  destination root, tkfmt-1 folder/name expressions with a debounced
  live preview, problems-only feedback, and a cancellable parallel run
  with persistent settings.
- Done: the ADR-0107 addendum makes conversion reachable from the local
  track context menu, restores the regular menu bar (hamburger button
  removed), and offers the app's saved naming layouts and destination
  roots inside the converter as one-click, still-editable choices.
- Done: ADR-0108 lands saved encoder presets: user-defined profiles in
  the workspace database (migration 27) listed beside the immutable
  built-ins, created from a prefilled editor where editing always
  saves a new profile, constrained to the qualified encode formats
  with one rate control, and deletable when saved.
- Done: ADR-0109 adds the resampling option: an optional target sample
  rate on the conversion request and scan (encoder constraints still
  apply afterwards, so Opus maps requests into its 48 kHz family),
  offered in the converter as a persisted "Resample" choice from Keep
  source rate up to 192 kHz.
- Done: ADR-0110 adds the bit-depth policy: an optional 16/24 target on
  request and scan that outranks the preset's sample-format hint where
  the encoder stores integer PCM, high-passed triangular dither
  whenever quantizing to 16-bit, and a persisted "Bit depth" choice in
  the converter — 24/96 sources convert to dithered 16/44.1 FLAC in
  one run.
- Done: ADR-0111 makes conversion and publication degrade gracefully on
  limited filesystems (NFS, sshfs, FAT): a shared no-replace publish
  ladder replaces the bare RENAME_NOREPLACE, xattr preservation manages
  the user. namespace only and tolerates unsupported destinations with
  problems-only notes, ownership refusals note instead of fail, and the
  whole path is proven live against the real NFS NAS — conversion,
  and a verified local-to-NAS move publication.
- Done: the ADR-0111 addendum degrades source locking the same way:
  exclusive flock falls back to shared, then unlocked, where lock
  emulation cannot express it (NFS needs write-open descriptors for
  exclusivity) — revision revalidation before every mutation carries
  correctness. The full publication executor suite and text tag commits
  now pass with their files living on the NFS NAS; the artwork undo
  chain on exchange-less filesystems remains a follow-up.
- Done: ADR-0131 carries one resolved cover per source into every
  qualified preset — native-FLAC pictures through the qualified inventory
  (role-level type and description preserved), other containers through
  the container-agnostic reader, then exact-basename sibling covers —
  embedded at mux time as FLAC `PICTURE`, ID3v2 `APIC`, or
  `METADATA_BLOCK_PICTURE` and reread byte-exactly before publication.
  Real-file tests round-trip each target independently with TagLib; the
  converter dialog exposes a persisted, default-on "Embed cover art"
  opt-out.
- Done: ADR-0132 adds the mirror-source-structure destination mode: the
  path planner derives relative directories and basenames byte-exactly
  from each source's location below the inferred deepest common source
  directory, keeping sanitization, containment, and collision checks;
  sources outside the root and shared-source logical items fail closed.
  The converter dialog exposes it as a persisted "Mirror source folders"
  checkbox that disables the naming expressions.
- Done: ADR-0133 treats every conversion as signal-changing: the
  mux-time transfer strips `REPLAYGAIN_*` and `R128_*` fields, the
  pre-publication reread verifies none survive, and real-file tests pin
  the stripping for every qualified preset. A bit-identical lossless
  transfer exception and automatic output rescanning remain follow-ups.
- Done: ADR-0134 adds the downsample-only rate cap (sources above are
  resampled to it, sources at or below keep their rate, encoder
  constraints still apply) and keep-source bit depth from the probed
  stored format (≤16 stored bits keep 16, everything else keeps 24).
  Both are per-item scan options with converter-dialog entries, and
  conflicting option pairs fail closed with real-file test coverage.
- Done: ADR-0173 adds Keep/Mono/Stereo channel policy and optional permanent
  Track/Album ReplayGain with peak protection, and makes saved presets restore
  the whole interactive job while recording backend versions. It reconciles
  every exit criterion and closes M8. One output per exact logical selection
  is the qualified output model; grouped chapter containers, merge-all output,
  a general DSP graph, multiple pictures, and automatic output scanning remain
  separately designed enhancements rather than incomplete variants.

### Exit criteria

- A preset fully reproduces codec, processing, naming, mapping, and concurrency
  choices with backend versions recorded.
- Mirror mode preserves the source hierarchy relative to a declared source
  root and rejects items outside it visibly.
- Cancellation/failure never damages existing destinations or publishes a
  partial file.
- Signal-changing operations remove or rescan stale ReplayGain metadata.
- Parallel work improves throughput without harming playback or UI budgets.
- Publishing into an MPD music root works with plain filesystem access and no
  protocol dependency.

## M9 — Melody endpoint and advanced listening (MPD authority)

### Objective

Turn the shared local playback engine into a first-class Melody streaming
output for Trackbench's MPD authority and add deeper listening features without
destabilizing either authority. The MPD controller uses the shared
`audio`/PipeWire libraries as a server-controlled output, not as an implicit
handoff to Local Queue playback.

### Work

1. Implement the current Melody agent protocol behind a versioned adapter,
   including registration, queue sync, play/preload, seek, volume, ReplayGain,
   keepalive, reconnect, and playback-clock reporting.
2. Stream original or negotiated/transcoded server media when a shared local
   root is unavailable; prefer safe direct access when configured.
3. Gapless preload and reconnect/resume behavior compatible with Melody's
   primary/multi-output rules.
4. Add advanced playback orders, optional DSP presets, MPRIS, media keys,
   notifications, and persisted statistics as separately gated capabilities.
   ADR-0135 delivered the MPRIS/media-key piece ahead of the milestone:
   `org.mpris.MediaPlayer2.trackknife` mirrors the active authority's
   transport with typed metadata, position/Seeked, and volume, and routes
   desktop commands through the same transport actions as the visible
   controls; notifications remain open.
5. Configurable MPD music-root mapping (per connection profile): resolve MPD
   queue/playlist URIs to local files below the configured root so Properties,
   Convert, and ReplayGain work directly on MPD-mode selections; a database
   update stays an explicit MPD action afterwards (ADR-0058). ADR-0112
   delivered the first piece: a global music-root setting plus explicit
   "Load as local files" from the MPD queue and server library into a
   local tab, and its addendum added the explicit companion: "Update
   this folder in MPD" on library nodes, scoped to the common directory
   prefix of the node's files. ADR-0113 added the library heading's
   A-Z / Latest toggle, ranking artists by database insertion time
   (MPD 0.24 Added, mtime approximation on older servers) through a
   sorted MPD search.

ADR-0174 completes the endpoint with a capability-gated Melody v2 adapter,
version-consistent queue synchronization, direct/stream source selection,
gapless preload, clock/advance reporting, and reconnect behavior. MPRIS,
media keys, and notifications were delivered ahead of this gate; album shuffle,
persisted listening statistics/resume, and a general DSP graph remain
separately scoped enhancements.

### Exit criteria

- Trackbench appears as an online/offline Melody output and survives daemon,
  network, and application reconnect scenarios without double advancement.
- Queue/version synchronization and primary-output clock rules pass fixtures
  derived from `../melody`.
- Direct-file and streamed playback select the correct source and produce the
  same visible track identity.
- Endpoint work does not regress stock MPD client behavior.

## M10 — Hardening, packaging, and releases

### Objective

Ship one maintainable primary Linux workspace trusted for listening and
collection work. The compatibility-shell migration outcome was resolved as
removal by ADR-0071.

### Work and exit criteria

- Transactional settings/database/tab/layout migrations and backup/restore for
  both authorities.
- Malformed-server/media fuzzing, sanitizers, fault injection, reconnect and
  long-running stress campaigns.
- Accessibility and keyboard/mouse workflow audit of both authorities.
- Complete dependency/license inventory and Qt LGPLv3 compliance.
- Native packaging and/or Flatpak with PipeWire, filesystem, and network access
  tested honestly for Trackbench.
- User manuals, protocol capability diagnostics, preset export, and release
  checklists.
- Packaged Trackbench completes MPD connect/browse/search/queue/list workflows
  and local play/tag/identify/ReplayGain/convert workflows without hidden setup
  or UI stalls.

### Completion evidence

M10 is complete. ADR-0175 defines consistent workspace backup boundaries;
restart-time restore includes versioned settings and retains a rollback database.
The UI exposes negotiated MPD capability diagnostics, and encoder and metadata
presets have versioned export formats. The Arch package and desktop file are the
supported native release surface; dependency/license and dynamic Qt LGPL
obligations are inventoried in `THIRD_PARTY.md`.

Development, optimized release, and ASan/UBSan suites pass all 67 tests. Focused
MPD/reconnect, Melody, playback, backup, and conversion tests also pass 25
consecutive iterations; fuzz, static-analysis, benchmark, and long-run soak
hooks remain release gates in CI and `docs/release-checklist.md`. Keyboard,
mouse, accessible-name/state, and authority-bound workflow coverage lives in the
offscreen UI suite, with hands-on stock MPD, Melody, and local real-file evidence
linked from `docs/m10-validation.md`. Every published artifact must still run the
release checklist against its exact checksum; completion does not claim Flatpak
or certify an untested future package.

## Beyond the first releases

Possible later work includes a local-tool-owned library index if direct
filesystem navigation stops scaling, explicit cross-authority integration
(opening a mapped server item as a local source and offering an MPD database
update after publication), autoplaylists and deeper queries, secure CD ripping,
internet radio, plugins, additional DSP, more format adapters, and a
capability-advertised transactional Melody import/upload destination if such a
protocol extension is actually implemented. None should bypass the shared
source, list, job, operation, expression, and performance contracts.

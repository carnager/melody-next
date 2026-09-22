# Architecture

## Status

Qt 6 Widgets, C++23 with a Qt-free core, the Linux/build baseline, focused
media/storage backends, utf8proc, `tkfmt-1`, the MPD-client-first direction,
the unified MPD/local Trackbench workspace (ADR-0058), Trackbench's versioned composed panel
layouts (ADR-0026), its semantic track-view layouts (ADR-0027), bounded
cue-sheet logical-track planning (ADR-0028), and persistent source-rate buffer
profiles (ADR-0029), and continuous PipeWire device monitoring with conservative
output recovery (ADR-0030), exhaustive container-chapter projection
(ADR-0031), explicit decoder selection plus tracker subsongs (ADR-0032), the
typed local metadata read model (ADR-0033), the staged metadata selection grid
(ADR-0034), bounded in-memory metadata drafts (ADR-0035), and the aggregate
metadata fields workspace (ADR-0036), plus exact ordered metadata value editing
(ADR-0037), the file-selection-driven Properties workspace (ADR-0038), its
dynamic metadata field vocabulary (ADR-0039), and ranked field-name completion
(ADR-0040), background complete Draft projection (ADR-0041), and revalidated
metadata write-plan/conflict preview (ADR-0042), plus the preservation-verified
prepared-copy FLAC text writer (ADR-0043), the journaled native-FLAC
commit/recovery executor (ADR-0044), and the provenance-aware dependent-state
cache transaction (ADR-0045), plus startup recovery presentation and bounded
retained-backup undo (ADR-0046), and cancellable bounded metadata Apply jobs
(ADR-0047), plus versioned previewed metadata transformation chains (ADR-0048),
persisted exact-value chains (ADR-0049), typed first-character metadata
capitalization (ADR-0050), and the persistent automatic tagging transformation
panel (ADR-0051), plus the tabbed tagging workspace and expandable change
preview (ADR-0052), and exact-value matching plus selection-order numbering
(ADR-0053), plus the unified preparation plan and reusable output profiles
(ADR-0054), and versioned output-profile persistence plus pure path planning
(ADR-0055), plus fresh filesystem preflight and a durable file-publication state
machine (ADR-0056), plus journaled same-filesystem file publication and recovery
(ADR-0057), revision-qualified all-occurrence relocation (ADR-0059), linked
same-filesystem undo (ADR-0060), and verified cross-filesystem publication
(ADR-0061), plus revision-qualified active-playback relocation (ADR-0062), are
joined by bounded file-publication Apply (ADR-0063) and typed keep-first
metadata transformation (ADR-0064), plus pasted rule-script translation and
conditional removal (ADR-0065), and explicit semantic/freeform metadata field
identity (ADR-0066), plus the immutable final-metadata/path review, qualified
Rename/Move UI, and unified file recovery/history (ADR-0067), and bounded
versioned multi-target metadata capture patterns (ADR-0068), plus strict
source-tag authority for path-only preparation (ADR-0069), canonical raw
transformation editing with dirty-state protection (ADR-0070), and strict
native typed-chain interchange (ADR-0072), plus journaled changed-content
destination artifacts (ADR-0073), and composed metadata/path publication
(ADR-0074), reused-target cache and batch-directory evidence (ADR-0075),
bounded typed artwork inventory (ADR-0076), and lazy read-only Properties
artwork presentation (ADR-0077), plus immutable preservation-verified native-
FLAC artwork planning (ADR-0078), and compact journaled native-FLAC artwork
publication/recovery (ADR-0079), and bounded Properties artwork review/Apply
with preservation-exact native block rewriting (ADR-0080); decisions are
accepted through ADR-0080.
Backend selection is not a capability claim; protocol and file behavior still
require fixtures and measurements.

## System shape

One repository builds a unified Trackbench workspace over shared internal
libraries. The former standalone Trackknife MPD shell was retired in ADR-0071.

```text
Trackbench: MPD Queue context       Trackbench: Local Queue context
  | typed commands, immutable view models, event subscriptions
Application services
  | MPD session | server library      | local playback | preparation | jobs
Core domain (shared)
  | source refs | metadata/MusicBrainz | list docs | tkfmt | jobs | plans
Adapters (linked per application)
  | MPD | SQLite | Melody agent        | filesystem/tags | FFmpeg | loudness
  |                                    | PipeWire
External/platform
  | MPD or melodyd                     | local files | Linux desktop/audio
```

The UI never owns sockets, decoders, tags, SQL transactions, or file writes.
Adapters never reach into widgets. The core can later serve a CLI without a
second interpretation of domain behavior. Trackbench links both adapters but
routes them only through the active authority; local mutation services reject
MPD selections by construction.

## Primary data flow

```text
Trackbench MPD context:
MPD server --command/idle--> MpdSession --> immutable server snapshots
                                      \--> tab/list and view services --> UI

Trackbench local context:
Local source --> playback: FFmpeg --> gain/DSP --> PipeWire
             \-> tags / MusicBrainz / ReplayGain / organize / convert plans
```

ADR-0120 places the existing asynchronous MPD search model and queue actions
in the Sources panel beside the persistent browse tree, using the panel layout
for geometry rather than an overlay over Track Lists. ADR-0122 adds the
`MpdLibrarySearchModel` tree presentation over the bounded search projection,
using the same tree view/delegate as local search. Complete-release expansion
and selection resolution use serial asynchronous controller requests, retain
persistent indexes, and reject results invalidated by a query change.

MPD is the primary library and live-queue authority for its context. Trackbench
persists authority-qualified profiles, workspace/list state, presets, jobs, and
journals in SQLite. ADR-0115 adds an optional local-library index in that store;
it is a rebuildable file-metadata cache and never mirrors the MPD database.

`persistence::LocalLibrary` owns folder configuration, revision-based scanning,
and bounded artist/album/track queries without Qt types. The Library panel
uses two bounded workers so queries can run alongside scanning. Metadata and
relocation transactions call its index-refresh helper before committing list
and cache state. See [local-library.md](local-library.md).

ADR-0126 adds bounded cleanup of confirmed missing index records after complete
manual scans, guarded by filesystem-device evidence, source revisions, and root
scan tokens. It also propagates MPD browse resets to retained search artwork,
invalidating old request tokens while preserving search rows and query text.

## Suggested modules

### `model`

Qt-free domain types: stable IDs, server/profile identity, MPD URI, raw local
path, remote/local source capability, logical segment, arbitrary ordered
metadata, MusicBrainz identities/credits, technical info, artwork, loudness,
queue occurrence, list item, revisions, and errors.

### `mpd`

`libmpdclient` adapter, connection/authentication, capability discovery,
command serialization, idle monitoring, reconnect generations, protocol-to-
domain projection, queue diffing, browse/search, output extensions, and a fake
server test harness. It contains no widgets and exposes no libmpdclient handles.

### `workspace`

Shared track/list document model. In the client it hosts the live MPD queue,
stored playlists, scratch lists, and saved lists over remote references; in
Trackbench it hosts local working lists over raw paths. Owns dirty state,
persistence, cross-tab operations, duplicate occurrences, snapshot
provenance, and supported action capabilities. The model is distinct from Qt
tab widgets. ADR-0123 adds bounded removal/reorder history to the local list
presentation model: detached occurrence snapshots and inverse permutations,
with persistent-index remapping and verified commit refresh of retained rows.
History replay uses normal dirty-list persistence and remains separate from
file-operation journals and MPD mutations.

ADR-0125 adds find in local lists and the MPD queue as one presentation controller
with a bounded worker and detached text batches. Core Unicode mappings and literal
substring matching operate on cached metadata/path snapshots; generation checks
reject results invalidated by edits, server queue refreshes, selection changes, or
authority switches. Find selects existing occurrences and owns no list, playback,
or persisted state.

### `lists`

ADR-0127 introduces a Qt-free planner for stable natural ordering over `tkfmt-1`
keys, reverse permutations, and exact logical-source duplicate removal. One UI
worker consumes bounded detached snapshots; list or authority changes invalidate
pending results. The model applies current plans through its existing history
and persistent-index remapping, and the workspace saves the resulting order.

ADR-0128 adds strict local M3U8 parsing/serialization and bounded file adapters.
A single UI-owned worker prepares imported rows and exports detached list
snapshots. Import publishes a complete named local list; export fsyncs a sibling
temporary file and atomically hard-links it to a new destination without replacing
existing files. Logical/remote references fail explicitly rather than losing
source identity. No database migration or GUI dependency enters `lists`.

### `titleformat`

The immutable versioned `tkfmt-1` parser/compiler/evaluator, dependency
extraction, diagnostics, and corpus. It is pure and is shared by views, sorts,
groups, transformations, and relative destination generation.

### `formats`

FFmpeg probe/decode/encode adapters. A narrow libopenmpt identity adapter
enumerates tracker subsongs that FFmpeg can select but does not enumerate;
FFmpeg remains the PCM decode boundary.

### `audio`

Local playback state machine, source resolver, FFmpeg decoder lifecycle,
gapless scheduling, PCM negotiation, ReplayGain/DSP stages, PipeWire buffer and
device output, and underrun diagnostics. A dedicated Qt-free playback worker
serializes source replacement, bounded decoder production, transport, seek,
PipeWire transitions, drain, and cancellation while publishing immutable
snapshots. The real-time callback performs no blocking I/O, allocation, SQL, or
UI work. ADR-0119 adds typed decoder ReplayGain values and a coalesced worker policy
that scales each source’s floating-point PCM before the ring, with exact Off
bypass and known-peak clipping prevention. A Qt-free lazy shuffle traversal
feeds the shell’s occurrence-anchored local Repeat/Random/Single/Consume
progression; these controls and preferences remain independent of MPD.
Named and exact duration profiles configure future ring allocations
without resizing live RT state. A persistent registry/default-metadata monitor
publishes device generations to the same worker; explicit-target loss pauses
without fallback and reconnects in place when the target returns. Trackbench's
Trackbench's Local Queue player and the M9 Melody endpoint each own a service
instance; the latter is a server-controlled output and never borrows Local
Queue authority (ADR-0174).

### `preparation`

Trackbench's local intake and capability resolution across metadata, artwork,
loudness, organization, conversion, playback, and publication. It turns a
local selection into typed operation inputs and readiness state without
becoming a canonical local library.

### `metadata`

Rich multi-source documents, MusicBrainz projections, TagLib/format-specific
adapters, the sparse staged-selection projection, field vocabulary and ranked
search, the bounded copy-on-write patch set and worker result projection, and
the physical-source-aware revalidated write-plan preview, plus the native-FLAC
preservation-verified prepared-copy writer. Versioned ordered transformation
chains, their exact add/copy/split/join and first-character capitalization
semantics, pure final-cell preview, and the independent Qt-free `tkcapture-1`
compiler/matcher remain in this module. One capture action may expose several
semantic and exact-freeform preview targets while still staging atomically.
The bounded cleanup importer also owns a verified canonical source projection
for representable typed actions; persisted typed actions remain authoritative.
The first artwork reader inventories native FLAC picture blocks and exact
configured sibling PNG/JPEG files as bounded revision-qualified records with
typed roles, exact native type, MIME, dimensions, SHA-256 content identity,
provenance, and duplicate linkage. ADR-0078 adds the distinct immutable native-
FLAC replace/remove plan and prepared-copy adapter. It rereads replacement
PNG/JPEG input from captured path/revision/hash evidence and proves the final
picture inventory, exact unrelated picture payloads, untouched text/unknown
blocks, and compressed audio without retaining encoded blobs in a plan.
ADR-0079 routes that plan through the shared unchanged-path metadata executor;
migration 23 stores only operation kind, ordinal/count/hash evidence, and
complete-inventory digests needed for recovery. Published artwork rereads feed
the existing all-occurrence revision/document refresh without making SQLite an
artwork cache. ADR-0121 advances same-workspace tag-source revisions only
through verified artwork commits, preserving text baselines, sparse edits, and
undo history through immutable revised-source snapshots. Local list publication
uses data-change notifications to retain playback occurrence identity.
ADR-0080 exposes exact embedded-row Replace/Remove through fresh
immutable review and a two-worker cancellable Apply job. Its native prepared
copy changes only the reviewed FLAC picture block and streams every unrelated
metadata block and compressed audio byte unchanged.
ADR-0081 extends the plan with native-FLAC append semantics and schema-24 Add
evidence. Embedded Copy donors remain compact revision/ordinal/hash input
evidence and are reread only while preparing each destination; they never
become temp files or SQLite blobs. The separate bounded Export service is a
non-mutating exclusive-output boundary over the same transient image reader
and therefore owns neither a mutation journal nor retained Undo artifacts.
ADR-0178 adds selection-expanded field allowlist/blocklist actions to the same
pure preview path; schema 34 and native JSON retain their bounded field lists.
Saved definitions cross the `persistence` boundary only as validated
declarative data. Each adapter
publishes independent
read/write/artwork/preservation capabilities;
decode or read support never implies write safety. Loudness is displayed beside
tags but remains a typed record with algorithm/provenance.

### `loudness`

libebur128 accumulators, album reducers, sample/true peak, grouping, scan plans,
and typed results. It consumes FFmpeg-decoded PCM independently of tag
writability.

ADR-0124 retains immutable stream/subsong/range inputs beside Properties
metadata rows and carries them into logical-source scans. A Qt-free staged
source flag prevents the physical metadata planner from publishing logical
ReplayGain/R128 values as whole-file tags. ADRs 0139-0145 route those values to
revision-gated CUE or sidecar carriers with journal and playback parity.

### `convert`

Versioned encoder presets with runtime FFmpeg capability probing and the
qualified single-file conversion primitive: optional permanent ReplayGain,
channel/rate/depth conversion through swresample, then FFmpeg encode into a
hidden temporary verified by full decode before atomic no-replace publication.
The bounded scan stays Qt-free; the UI layers destination planning and complete
saved-job snapshots over it (ADR-0173).

### `operations`

Shared plan/preview/commit/journal framework for tags, artwork, files,
conversion publication, verification, and loudness writes. Mutations acquire
per-source locks and revalidate revisions. The first implementation serializes
native-FLAC sources, journals exact raw paths and recovery identities through a
persistence interface, atomically publishes a verified sibling, retains the old
inode as a backup, requires an idempotent dependent-state commit, and rolls back
or conservatively escalates interrupted states without depending on Qt. A
separate backup lifecycle supports verified atomic-exchange undo, explicit
release, startup maintenance, and restart recovery without conflating backup
retention with publication state. A Qt-free two-worker Apply orchestrator now
consumes only wholly ready plans, serializes progress delivery, stops new
admission on cancellation, and preserves ordered per-source partial results.
Trackbench composes final tag context and file paths into one immutable
preparation review. Exact raw-path/revision pairing now dispatches metadata-only,
path-only, combined metadata/path, and no-change sources through one bounded
preparation Apply. Combined work prepares a verified destination artifact and
supplies its reread document to one durable all-occurrence relocation and cache
refresh. A path-only review carries no metadata plan or synthetic context and
evaluates from the captured revision-qualified source tags. Executor-proven
directory creation remains batch-owned if later work rolls back, while an
unexplained path appearance still fails admission (ADR-0075).

ADR-0184 adds a separate descriptor-relative folder-image publisher beside the
embedded-artwork committer. Captured storage policy extends ready artwork plans,
not draft intents. Schema 38 shares the metadata journal and backup lifecycle
but routes folder recovery separately from media-document projection. A shared
folder is one destination; conflicting incoming hashes block review. Folder and
media files remain distinct journaled publications with explicit partial-failure
reporting. The Fields thumbnail consumes the existing bounded artwork worker.

### `jobs`

Bounded resource-aware scheduler, dependencies, cancellation, progress,
structured per-item results, retry, history, and UI events. Network sessions,
audio realtime, CPU decode/analysis, encode, local I/O, and mutation have
separate limits rather than one undifferentiated pool.

### `persistence`

SQLite migrations/repositories for profiles, workspace, list documents and
snapshots, presets, jobs, statistics, caches, and operation journals. Writes are
short and transactional; sockets, decode, and filesystem work never occur
inside a database transaction. Verified metadata publication refreshes every
matching local occurrence and a raw-path source cache in one idempotent
transaction; provenance keeps logical-track overlays separate, and the source
cache prevents a delayed ordinary workspace save from restoring stale fields.
Combined publication relocates all occurrences and installs the verified target
document and revision in that same transaction, while retaining logical
CUE/chapter/subsong/annotation overlays. Schema-22 idempotency evidence records
whether a relocation included this metadata refresh. A historical target cache
with no persisted owning occurrence is superseded atomically when the pathname
is reused; it is durable correctness data rather than permanent path ownership
(ADR-0075).
The operation journal also guards retained-backup state and unique reverse
operation identities for crash-recoverable undo. Apply first persists a
UI-captured workspace snapshot on this worker so a newly opened occurrence
cannot race the ordinary save debounce.
ADR-0175 adds a Qt-free online-backup boundary over that database: SQLite
captures committed WAL state into an exclusive destination, which is reopened
read-only for integrity and Trackknife-schema validation before success is
reported. Restore remains a restart boundary so no live repository connection
can observe a database being replaced underneath it.

### `ui`

Qt Widgets presentation split into a shared component library (virtualized
track views, tab workspace, transport row, command palette, toasts, settings,
shortcuts, and the versioned panel- and track-view-layout models/validators)
plus one primary Trackbench shell: authority-bound MPD connection,
server-browser/search, queue tabs, and outputs alongside the composed-panel
renderer, folder navigation, local list tabs, tag grid, previews,
preparation-operation history and metadata/file reconciliation evidence, and
job center.
Panel layout
owns only placement and sizing of registered instances, never application
state. ADR-0052 hosts each captured metadata Properties task as a temporary
protected tab in the Track Lists surface while excluding it from durable list
documents. ADR-0077 keeps the existing Properties file selector as the shared
scope for sibling Fields and Artwork sections. Artwork inspection starts only
while that section is active, runs through one cancellable worker, rejects more
than 64 distinct sources visibly, and retains neither encoded pixels nor
inventory state in SQLite. Track-view presentation separately owns semantic
column placement and group geometry, never queue/list occurrence state. UI consumes C++
`QAbstractItemModel` and controller objects; no
`QQuickWidget` hybrid.

## Identity and source records

```text
ProfileId       stable MPD connection identity
RemoteTrackKey  ProfileId + exact MPD URI + optional segment identity
QueueItemKey    ProfileId + MPD song ID + server queue generation
LocalSourceId   stable internal UUID + observed filesystem identity
ListId          stable Trackknife tab/list identity
JobId           stable job identity
Revision        server/list/source revision appropriate to the authority
```

An MPD URI and local raw path are different representations even when a profile
root maps between them. A remote snapshot may outlive connectivity. A local
revision uses filesystem identity/size/mtime plus hashes where needed for safe
mutation.

## Conceptual persistence

```text
mpd_profiles(id, name, endpoint, secret_ref, local_root, reconnect_policy)
workspace_panels(id, type, config, schema_version)
lists(id, kind, name, profile_id?, dirty, revision, config)
list_items(list_id, ordinal, source_kind, source_ref, logical_ref?, segment?, snapshot)
view_presets(id, name, version, definition)
local_sources(id, raw_path, fs_identity, size, mtime, revision, availability)
local_metadata(source_id, field, ordinal, value, provenance, revision)
loudness(source_id/segment, grouping, gains/peaks, algorithm, revision)
jobs(id, kind, state, summary, resumable_payload)
operation_journal(id, plan, steps, state, recovery_payload)
metadata_operation_backups(operation_id, state, undo_id?, timestamps, failure?)
```

The live MPD queue is not persisted as a second authoritative list. Its latest
snapshot may be cached for fast/offline display, clearly marked stale. Scratch
and named Trackknife lists are authoritative application data.

## Events and consistency

Typed events carry IDs/revisions or small immutable snapshots:

```text
MpdConnectionStateChanged / MpdCapabilitiesChanged
MpdStatusChanged / MpdQueueChanged / MpdDatabaseChanged
MpdStoredPlaylistsChanged / MpdOutputsChanged
TrackListChanged / ActiveListChanged
LocalSourceChanged / MetadataChanged / LoudnessChanged
MpdPlaybackStateChanged / LocalAuditionStateChanged
JobProgress / JobFinished / OperationNeedsReconciliation
```

Session generations reject stale network results. Coalesce server and job
bursts so changes do not create unbounded UI redraws. Live-queue conflicts
refresh server truth; local list edits remain transactional and undoable.

## Operation lifecycle

```text
Draft -> Resolve inputs -> Plan -> Previewed -> Revalidate -> Journal
      -> Execute -> Verify -> Commit state -> Complete
                                \-> Partial / Needs reconciliation
```

Plans are immutable after preview. Capability resolution reports remote-only,
local, mapped-local, writable, and decodable sources before offering actions.

## Concurrency rules

- The UI loop receives only small immutable messages.
- Each active MPD session has bounded serialized command and idle workers.
- Non-idempotent protocol mutations are not automatically replayed after an
  ambiguous disconnect.
- PipeWire real-time work has the highest isolation and shares no blocking lock
  with UI, network, decoder, or operation workers.
- CPU-heavy scans/conversions use bounded pools and account for FFmpeg's own
  codec threads.
- Only one mutation holds a physical source lock; pending writers do not starve.
- Cancellation crosses every backend boundary and updates visible state quickly.

## Backend decisions

- **MPD:** dynamically linked `libmpdclient >= 2.22`, generic pair parsing for
  unknown and advertised extension fields, no handles outside the adapter.
- **Decode/encode:** FFmpeg libraries, never an `ffmpeg` subprocess for playback.
- **Metadata:** TagLib plus format-specific adapters. The generic property
  reader is active; native-FLAC text has preservation-proven prepared-copy and
  headless journaled commit plus bounded Apply support (ADRs 0043–0047).
  ADR-0048's versioned transformation chains stage through the same draft and
  write-plan boundary; ADR-0049 persists validated definitions and adds exact
  add/copy/split/join actions, while ADR-0050 adds typed first-character
  capitalization. ADR-0051 composes checked saved definitions into a temporary
  draft immediately before the same immutable write-plan boundary. ADR-0053
  adds complete-value exact remove/replace and captured-file-order numbering
  without folding grouping or capture grammar into schema 1. ADR-0064 adds a
  bounded Unicode-scalar keep-first action without assigning it date parsing
  semantics. ADR-0065 translates a bounded Picard-style paste subset into
  ordinary typed actions and adds versioned conditional field removal without
  adding mutation to `tkfmt-1`. ADR-0066 permits semantic aliasing only through
  explicit adapter tables, exposes other properties as independently mutable
  freeform native fields, and carries exact-native imported deletion through
  preview, write, journal, and recovery. ADR-0070 adds a deterministic raw
  cleanup-source projection, live translation, and explicit dirty-state
  protection while keeping typed actions as the only persisted authority.
  ADR-0072 losslessly imports and exports the complete current typed chain in a
  strict versioned JSON envelope without transferring catalog identity or
  automatic state.
  ADR-0078 separately qualifies native-FLAC embedded-picture replace/remove at
  the plan and verified prepared-copy boundary. ADR-0079 adds compact durable
  complete-inventory evidence and reuses the locked unchanged-path publication,
  rollback, recovery, undo, and all-occurrence refresh lifecycle. ADR-0080 adds
  Properties selection, immutable review, bounded progress/cancellation,
  fresh-retry behavior, and exact native block rewriting. Other format artwork
  writers still require real preservation tests.
- **Loudness:** libebur128 behind Trackknife-owned ReplayGain policy.
- **Preparation:** ADR-0054 composes tag persistence, qualified ReplayGain
  storage, and filesystem rename/move into one immutable per-source review and
  recoverable Apply boundary. Versioned relative output layouts remain separate
  from explicit raw-path destination roots and are reusable by conversion.
  ADR-0055 persists those contracts in migration 13 and adds a bounded Qt-free
  path planner over final metadata, captured source revisions, and an explicit
  filesystem snapshot. It owns lexical mapping and collision diagnostics only;
  ADR-0056 adds a no-symlink live preflight and classifies same-filesystem rename
  versus cross-filesystem copy without mutation. Migration 14 durably separates
  target preparation/publication, dependent-state commit, and source removal.
  ADR-0057 qualifies locked descriptor-relative no-replace rename, exact
  rollback, and idempotent startup recovery; ADR-0059 supplies concrete guarded
  dependent path state, ADR-0060 adds linked same-filesystem undo, and ADR-0061
  adds bounded metadata-preserving verified cross-filesystem copy, no-replace
  publication, source removal, and recovery. ADR-0062 adds a serialized exact-
  revision audio-binding barrier and compensates it if the durable list/cache
  transaction fails. ADR-0063 adds ordered bounded file batches, fresh per-
  source admission, monotonic progress, partial results, cancellation, and
  shared-directory topology coordination. ADR-0067 exposes qualified path-only
  Rename/Move through one final-metadata/path review, persists the captured
  workspace before bounded Apply, refreshes visible/player references, and
  connects both publication recovery state machines plus same-filesystem undo
  to the preparation-operation history. ADR-0069 excludes manual drafts and
  checked automatic chains from path evaluation whenever Save tags is off and
  makes a metadata-bearing path-only plan structurally invalid. ADR-0074 adds
  exact metadata/path pairing, bounded changed-artifact Apply, atomic
  metadata-plus-relocation persistence, and published-artifact recovery.
  Artwork mutation and cross-filesystem undo remain to qualify.
- **State:** SQLite with explicit reversible development migrations, including
  normalized ordered schema-1 metadata transformation definitions owned by the
  serialized persistence worker, including their automatic tagging policy
  flags, exact matching payloads, numeric arguments, and dialect-qualified
  conditions and exact-native removal identity (ADRs 0049–0053 and 0064–0066),
  plus separately versioned output-layout and raw destination profiles
  (ADR-0055) and file-publication recovery evidence (ADR-0056).
- **Local output:** direct PipeWire with a narrow fallback-capable interface.

## Implementation sequence

Steps 1–5 below were completed while establishing the separate authority
backbones. ADR-0058 now places them in one primary Trackbench workspace without
combining their queues or controllers:

1. ~~MPD domain snapshots, session, browse/search, and reconnect tests.~~
2. ~~Persistent queue/list tabs and the polished client workspace.~~
3. ~~FFmpeg decode, bounded playback core, PipeWire output, local ingestion.~~
4. ~~Shared-library build restructure and the `trackbench` executable.~~
5. ~~Migrate local playback/ingestion into Trackbench and retain separate MPD
   and local controllers.~~
6. Finish the Trackbench tag grid and mutation framework, including artwork
   management; then add MusicBrainz providers.
7. Parallel ReplayGain, then converter/resampler and organized output.
8. ~~Melody endpoint in Trackbench's MPD authority (ADR-0174).~~ Continue with
   hardening and packaging (the compatibility shell was retired in ADR-0071).

ADR-0115 authorizes the optional local library. A plugin SDK and elaborate
theme system remain deferred until both authority experiences are excellent.

Per ADR-0194, tag editors own their metadata and selection models directly. Each
creates one file-list widget that the workspace hosts in its Files sidebar;
there is no second mirror view. Leaving a tab returns its widget to the editor.

## Shared dynamic playlists (ADR-0195)

ADRs 0214–0215 add authority-owned album traversal and history query facts.
Local album keys are captured in bounded UI batches and the Qt-free traversal
is prepared on a worker; generation checks prevent stale list plans from being
installed. Melody owns its own album cycles and pending Repeat occurrence IDs.
No list order is mutated by playback modes. Explicit `HISTORY(...)` predicates
consume a consistent local-library/history snapshot or translate to advertised
Melody history filters. Whole-album aggregates precede predicate narrowing;
history invalidations refresh open dynamic-rule previews in their own authority.

ADR-0216 adds numeric `SORT HISTORY` keys and current-tab history evaluation.
Local source observations, statistics, and album aggregates share one SQLite
read transaction on the query worker. Melody owns bounded `melody_list_search`
evaluation over captured list occurrences and library-wide history; the client
does not intersect a truncated library response. Capability gates distinguish
history filtering, ordering, and list scope. Dynamic result actions reuse
source-qualified tools and transfer helpers. Definition-owned views copy outward
only; playback and manual membership edits use stable ordinary-list snapshots.

`DynamicPlaylistService` owns definition evaluation, provider requests, bounded
matching, cancellation generations, limits, and shuffle. The editor uses the
same service for both authorities. Adapters evaluate typed `CompiledTkq`
queries on a local-library worker or the serialized MPD session. Results are
homogeneous local or remote vectors; presentation uses the existing local and
MPD track models and shared track-view engine. The service never mutates a
queue. Opening a snapshot crosses into the existing authority-specific list
creation path only after an explicit user action.

ADR-0217's built-in search catalog and parameter-to-tkq generation live in the
Qt-free query module. Search presents grouped actions and typed prompts, then
uses the unchanged asynchronous search/saved-query pipeline. Remote menu
availability reuses the execution translator and advertised scope capabilities;
presets never own library facts or playback state.

## Up Next (ADR-0196)

The shared request panel renders authority-specific pending models. Local
`audio::RequestQueue` owns occurrence IDs and one pending-edit undo; the local
audio worker publishes occurrence tokens for gapless handoffs. Its original
list traversal remains suspended until the FIFO drains. Persistence uses a
versioned payload on the existing serialized UI-state repository. Melody owns
remote requests and continuation independently of client lifetime; typed
revision-qualified commands and snapshots cross the MPD session boundary.
Stock MPD never runs an emulated client-owned progression scheduler.

## Last.fm (ADR-0197)

ADR-0207 also uses the pure accounting primitive for independent local listening
history. Only qualified occurrences cross the existing serialized persistence
worker; schema-40 source identities follow verified publications in the same
transaction as lists and caches. Collection does not depend on Last.fm accounts
and does not observe the Melody endpoint or MPD transport.

Qt-free `core::ListenAccounting` consumes occurrence identity, position, playing
state, and monotonic samples. The local audio worker publishes a playback
instance on loads and gapless handoffs. Trackbench forwards only local-player
samples, independently of the selected tab, to a dedicated LastFmService worker
that owns HTTP and private file persistence. Account UI is shared with Melody
through typed command/reply envelopes on the serialized MPD session. Melody's
separate Go service samples only its primary output; provider network work never
holds queue/audio locks. There is one bounded durable outbox per authority and
no scrobbling in endpoint playback or stock-MPD clients.

Per ADR-0198, server queue and named-list track menus use one selection and
construction path. Shared actions resolve the source view instead of reading
MPD Queue implicitly. Backend addressing retains the existing unnamed-context
and named-playlist commands. Priority resolves selected positions/URIs to stable
occurrence IDs only in the active playback context; inactive lists cannot
accidentally mutate another list's priorities. Whole-list management remains on
the list/tab surface.

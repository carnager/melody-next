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

**The seam, measured.** `BenchMainWindow` carries 260 members; the three
playback translation units touch 98 of them. 33 are widget-typed and stay
(`QAction`, `QToolButton`, `QLabel`, `QSlider`, the up-next dock). The
remainder is the service: `local_requests_` (`audio::RequestQueue`),
`playback_order_`, the playback anchors and modes (now `anchors_` and
`local_modes_`), `playback_row_`, the album grouping state (now
`album_grouper_`), `local_listen_accounting_`, the `resume_*` pair,
`advance_pending_`, `last_requested_next_`, `queued_request_` /
`requested_request_`, plus the device and audio members (`player_`,
`melody_endpoint_`, `selected_device_`, ReplayGain preamps, buffer profile).
There is no `bench_transport.hpp` — these are all `BenchMainWindow::` methods
split across files, which is why the coupling never got noticed.

**Progress.** The identity work and the policy extraction are done; what
remains is orchestration. Delivered so far:

| In Qt-free `src/audio` | Covering |
| --- | --- |
| `PlaybackAnchors` | document, current entry, the two in-flight transition anchors, request return point, decode target |
| `PlaybackModes` | repeat/random/album-random plus the single and consume tri-states and their one-shot decay |
| `TrackSource` | path, selection within it, span — previously stuck in a Qt header |
| `PlaybackList` + `adjacent_playback_row` / `automatic_playback_row` | the advance rules, against a three-question view of a list |
| `resumable` / `resume_position_ms` | the resume predicate and its offset arithmetic |
| `AlbumGrouper` | the album grouping rule and its size limits |

All of it is exercised by `tests/playback-state`, which links
`Trackknife::Audio` only; `ldd` confirms it pulls in no Qt. That is Phase 0's
"done when" holding for the policy half.

**What Phase 0 does not cover, contrary to the list above.** Up-next, the
resume *checkpointing* flow, listen qualification and the chunked album walk
are still in `BenchMainWindow`, and they are not more pure functions waiting to
be found. What is left is orchestration: `QFutureWatcher` lifecycles,
`QSettings`, JSON persistence, status reporting, and the timers that drive the
extracted policy. Moving those needs the service to own its own scheduling and
to *tell a client* to do something — which is the Phase 1 client-facing API and
the Phase 2 request/response shape, not this phase. Phase 0 should be read as
"playback policy leaves the widget layer", with the orchestration following its
own interface once that interface exists.

**The blocker was how playback position is stored — now resolved.** Five
members held it as `QPersistentModelIndex`, assigned straight off the view
model (`playback_index_ = tab.model->index(row, 0)`) and read back as `.row()`
in 23 places. A `QPersistentModelIndex` *is* a pointer into a Qt model: it
survives row insertion and removal precisely because the model maintains it. A
core-owned service cannot hold one, and it certainly cannot send one over a
socket. Replacing them with entry identities had to come before any code moved;
the extraction was mechanical afterwards and impossible before.

Four of the five are now identities. `followed_playback_index_` deliberately
stays a model index: paired with `followed_playback_view_`, it tracks which row
to scroll into view, which is genuinely view state and not playback state.

Converting them exposed a real bug rather than only moving code. `applyMetadata`
and `applyProbeRows` replace a row wholesale with a freshly built one from the
probe, carefully preserving `logical_reference`, `selection`, `segment` and
`raw_path` — everything that must outlive a probe. Identity was not on that
list, so background enrichment silently reassigned it and anything anchored to
that row stopped resolving mid-playback.

**This is the same problem in three places, none of them MPD.** Stable
queue-entry identity is what Phase 0 needs internally, what Phase 2 must put on
the wire, and what tab unification needs so an entry is addressable
independently of its row. If the Phase 5 bridge is ever built it derives MPD
song IDs from this, but that is not a reason to have it.

Settled in [ADR-0221](adr/0221-entry-and-track-identity.md): an opaque
`core::StableId` per entry, and a separate metadata-derived track identity that
the entry carries. Phase 0 implements the entry half; Phase 6 migrates the
existing rating and listening schemes onto the track half.

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

**Tabs become one kind of thing.** Today there are two parallel structures with
the same shape and different model types:

```cpp
struct ListTab {                        struct MpdPlaylistTab {
    persistence::ListDocument document;     QString name;
    LocalListModel* model;                  quick::MpdQueueModel* model;
    QTableView* view;                       QTableView* view;
    ui::TrackViewLayout view_layout;        ui::TrackViewLayout view_layout;
};                                      };
```

— held in separate vectors (`list_tabs_`, `mpd_playlist_tabs_`), fed by
separate tab bars (`local_source_tabs_`, `mpd_source_tabs_`), built by separate
code paths (`buildWorkspace` / `buildMpdWorkspace`), and served by 54 distinct
`*Mpd*`-suffixed methods shadowing local equivalents.

This is not a merge. `MpdQueueModel` exists only because Trackknife is
currently an *MPD client* when talking to the Go daemon; once remote means
protocol v1 to a remote engine, Trackknife is never an MPD client again and
that half is **deleted**, not abstracted. Local versus remote stops being a
branch in feature code and becomes which profile the connection points at.

**But that deletion cannot happen in this phase.** MPD is how a remote library
is reached today — 295 `mpd_controller_->` call sites — and protocol v1 does
not exist until Phase 2, with remote transport arriving in Phase 3. Deleting
the MPD client half now would remove remote access with nothing to replace it,
which violates the rule that every phase leaves a working application.

So tab unification splits across phases:

- **Now**: the parts that do not depend on a protocol. The tagger stops being
  a tab (below). The structural groundwork — one tab type with a source
  descriptor and a refresh policy — can be prepared with both models still
  behind it.
- **After Phase 3**: the deletion, once a remote profile can reach an engine
  over the wire and the MPD client half has a replacement.

Phase 1's "done when" is amended accordingly: it does not include *one tab
type serves every list*, because that outcome depends on transport this phase
does not deliver.

A tab is then one thing: **a set of track references, an order, and a view
layout.** Static lists, saved searches, dynamic playlists, one-shot search
results and browse results differ only in how the set was produced and whether
it re-evaluates — a source descriptor and a refresh policy on one struct, not
five tab types. Search results in particular are not special: they are tracks
in a tab and play like any other.

This depends on the Phase 0 entry identity, because a tab entry must be
addressable independently of its row.

**The tagger is the exception, and stays a window.** It is already a non-modal
`QDialog` (`metadata_properties_dialog.cpp`, 3,778 lines, plus a 1,514-line
grid model). It should not become a tab: every other tab is a list of playable
tracks, while the tagger is an editing surface holding staged uncommitted state
with its own commit/cancel lifecycle, and tabs get closed casually. It should
also stop being a `QDialog` — Esc-closes-and-discards and button-box semantics
are wrong for staged edits across a multi-track selection. Target: a top-level
window with its own toolbar, explicit apply/revert, remembered geometry, and
several openable at once. Phase 2 confirms the shape, where it becomes a job
submitter — stage, submit, stream progress, handle partial failure — which is a
window with a task, not a view of a collection.

**Measured, and the direction of the dependency is the whole point.** The rule
is: *the UI asks the core to do something; the core owns the database, looks
things up, and hands back what was asked for.* Today it runs the other way.

`src/persistence` has no front door at all — `list_repository.hpp`,
`local_library.hpp` and friends are the raw classes, and whoever wants data
opens a database. Two kinds of caller do:

```sh
grep -rn 'LocalLibrary::open'   src/bench      # 6 sites, 3 files
grep -rn 'ListRepository::open' src/bench      # 1 site
```

`local_library_panel.cpp`, `dynamic_playlist_service.cpp` and
`search_dialog.cpp` each open the SQLite database **by path** on a worker
thread and query it. A remote client has no database path, so none of this can
cross a process boundary.

**`ui::ListPersistenceService` is not the pattern to copy.** It looks like the
missing service — 767 lines, async callbacks, every list operation behind it —
but it lives in `uicommon`, which links `Qt6::Widgets`, and it *holds the
repository*: `std::optional<persistence::ListRepository> repository` opened
from a `database_path` it owns, serialized on its own `QThread`. That is a
threading wrapper around UI-owned persistence, not a client of a core-owned
engine. Building a second one for the catalogue would double down on the wrong
side of the boundary.

So Phase 1 is not "add another service beside the existing one". It is:

- **the core grows a front door** — an engine-side object owning
  `ListRepository` and `LocalLibrary`, exposing operations as requests with
  results, with no Qt in its interface;
- **`ui::ListPersistenceService` becomes a client of it** rather than the owner
  of the database, keeping its async callback surface so the widgets above it
  do not change;
- **the six catalogue opens become requests** through the same door.

That ordering matters: the door has to exist before either caller can move, and
its shape is what Phase 2 serialises. Getting it right here is the difference
between a protocol that falls out of the interface and one bolted onto it.

The twenty `src/bench` translation units that include a persistence or
operations header are mostly *type* coupling — `ListDocument`, `ListItem`,
`LocalSourceRevision` crossing as values. That has to go eventually, but it is
benign in-process and says nothing about who owns the data, so it does not gate
this phase.

**Started.** `src/engine` exists with `engine::Catalogue` as the first front
door: `filter_paths`, `cached_tracks`, `artwork_source`, `history_facts`. Four
of the six catalogue opens now go through it, in `dynamic_playlist_service`,
`search_dialog` and the panel's artwork loader. Calls are synchronous and
each opens its own connection, exactly as the callers did inline; threading
stays with the caller for now, and becomes the protocol's concern in Phase 2.
Keeping the connection strategy behind the door is the point — it can become a
pool, or a socket, with no caller changing.

**All six catalogue opens are gone.** `engine::Catalogue` names ten
operations — `roots`/`add_root`/`remove_root`, `query`/`paths`/`filter`/
`filter_paths`, `cached_tracks`, `ratings`/`set_rating`, `artwork_source`,
`history_facts`, `scan` — and the library panel's task queue now hands its
work the door instead of the database.

On `scan`: it is long, mutating, cancellable and progress-reporting, which
ADR-0220 calls a **job**. That shape already exists at the call site, assembled
from Qt parts — a pool submits, a timer polls atomic counters, a token cancels,
a watcher delivers the result. Passing it through the door keeps that shape and
takes the database path out of the UI, which is what this phase is for. Phase 2
changes where the thread lives and whether progress is pushed rather than
polled; it does not change the shape here. Building engine-owned threading now
would duplicate the caller's pool and design the job machinery without the
socket that is its actual requirement.

**The gate is met.** No database open remains outside the engine:

```sh
grep -rn 'LocalLibrary::open\|ListRepository::open' src   # only src/engine, src/persistence
```

`engine::Workspace` is the other half of the door — lists, connection
profiles, view presets, saved transformation/output/encoder settings, local
listening history and saved searches. `ui::ListPersistenceService` now holds a
`Workspace` rather than a `ListRepository`: it keeps the thread it serialises
on and the callback surface the widgets above it expect, and has stopped being
the owner of a database. Its 23 operations were already named and well chosen,
which is why this was a relocation rather than a redesign.

The two doors differ in one visible way. `Catalogue` opens a connection per
call, matching what its callers did inline; `Workspace` holds one open,
because it is written on nearly every user action. That is a choice behind the
door, not something a caller sees — which is the property that lets either
become a pool or a socket later.

Still open in this phase: the type coupling (twenty translation units passing
`ListDocument`, `ListItem` and friends across the boundary as values) and tab
unification. Neither gates the ownership move that just landed.

Done when: nothing outside the engine opens a `LocalLibrary` or
`ListRepository`, and the UI reaches both only by asking the core. One tab type
serving every list is **not** part of this phase — see the sequencing note
above; it needs transport that arrives in Phase 3. The header-include sweep is
tracked separately.

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
- **Stable queue identity on the wire.** A playback position and a queue entry
  must be expressible without reference to a client-side row, because rows
  move. This is the same identity Phase 0 introduces to replace
  `QPersistentModelIndex`; here it simply has to survive serialisation. The
  requirement is internal and holds even if nothing else is ever built on it.
  If the Phase 5 bridge does get built it derives MPD song IDs and
  `plchanges` versions from this plus a monotonic queue version — worth
  checking the shape against `queuePosByMPDID` / `queueIDs` / `queueVersion`
  at design time, since it is free to allow for and expensive to retrofit.

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

# Feature matrix

Last reconciled: 2026-09-27, through ADR-0236, for the engine and playback
section; the other sections were last reconciled on 2026-09-16 through
ADR-0174 and may still describe the two-authority model where they mention
an MPD queue or authority -- read that as another engine. The application is
Trackknife (`src/bench`, installed as `trackknife`), a client of engines
([unified engine](unified-engine.md)).

Use the [roadmap](roadmap.md) for implementation priorities and
[MILESTONES.md](../MILESTONES.md) for capability gates and historical evidence.
M5-M10 are complete; artifact-specific release acceptance remains governed by
the release checklist.
An implemented feature does not by itself close a milestone.

Development update (2026-09-22): ADRs 0204–0208 add local listening-history
collection and optional columns, a curated workspace command palette, and dynamic-rule
invalidation on local index changes. ADR-0210 adds bounded paused resume for
local lists and hardens Melody's server-owned resume. ADR-0211 extends paused
resume to interrupted Up Next requests in both authorities. ADR-0212 adds
one-shot album shuffle to local lists and Melody's active unnamed queue.
ADR-0213 extends Melody album shuffle to named/scratch lists and the stashed queue.
ADRs 0214–0215 add continuous album playback and explicit listening-history
library queries in both authorities. ADR-0216 adds numeric history result ordering,
current-tab queries, and dynamic result interaction parity (stable playback/edit
snapshots, copy-only drag, source-qualified actions and refresh anchors).
Persistent autoplaylist tabs and custom
library grouping remain open.

## Status vocabulary

- **Implemented:** available in the current workspace or the named core service,
  within the stated scope. This is not a claim that every related requirement
  or format is complete.
- **Partial:** usable implementation exists, with identified missing behavior.
- **Backend only:** reusable support exists without the complete current UI.
- **Open:** not implemented in the reviewed workspace.
- **Needs verification:** a source-review concern requires a regression test.
- **Deferred:** outside the immediate roadmap; proposals are not compatibility promises.

## Workspace and playlists

Playback tabs carry an explicit “Active” suffix and theme-accent text color independently of the selected
browsing tab, in both local and server contexts. Stop and Pause retain that
active list; playing another list moves the marker. Workspace offers a persisted,
opt-in Cursor follows playback toggle and Jump to playing (Ctrl+J). Following
selects/reveals new playing rows only in the visible playback list; Jump opens
the playing list for the selected authority. Ordinary external MPD edits
update the active server list (ADR-0199).

Diagnostic tracing is opt-in via `--debug` or explicit `QT_LOGGING_RULES`;
ordinary startup suppresses the `trackknife.debug` trace category while
retaining warnings and errors (covered by the `debug-log` regression test).

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Local working-list tabs | Implemented | Persistent scratch/named lists, rename, save, pin, duplicate, reorder, dirty-close protection, row removal, and cross-list copy/move and drag/drop. Duplicate occurrences and raw paths survive persistence. Track context menus separate playback, metadata tools, library navigation, list transfers/removal, ordering, and edit history into groups. ADR-0167 adds New tab… to both transfer menus and tab-strip drops (move by default, Ctrl-copy). |
| Local list editing tools | Implemented | Per-tab removal/reorder undo/redo with shortcuts, menus, bounded session history, and playback-preserving index updates is implemented (ADR-0123). ADR-0127 adds cancellable whole-list natural sorting with tkfmt-1 presets/custom expressions, reversal, and logical-source-aware keep-first duplicate removal, each with named undo/redo. ADR-0176 adds bounded add/replace and destination-side copy undo plus one workspace-level undo/redo transaction for the latest cross-tab move. Tag-draft undo remains separate. See [roadmap 2](roadmap.md#2-lists-and-library-views). |
| Find within track lists | Implemented | ADR-0125 adds Ctrl+F, next/previous matches, wraparound, progress/cancellation, and Escape to close in local lists and the MPD queue. ADR-0142 widens the haystack to every metadata value at every provenance, formatted durations, MPD `audio_format`/unknown pairs, plus the escaped paths/server URIs — still bounded worker batches without filtering, playback changes, or server commands. Local codec/bit-rate/sample-rate search awaits probe-technicals retention. |
| Portable playlists | Partial | ADR-0128 adds cancellable M3U8 import into a new local list and whole-local-list export to a new file. Relative paths, duplicate/offline occurrences, EXTINF labels/durations, and escaped raw filenames are preserved. Logical selections and remote references fail explicitly; existing files are never replaced. M3U encoding policy, XSPF/PLS, and replacement workflows remain open. |
| Metadata service settings | Implemented | Settings → Metadata services exposes AcoustID application/client and Last.fm API keys with masked entry, registration links, Save/Cancel, and clearing (ADR-0195). AcoustID also has a reveal control. MusicBrainz text search remains credential-free; network lookups remain explicit. Settings pages scroll at smaller sizes and explain their save behavior (ADR-0185). |
| Committed search-result tabs | Implemented | ADR-0140: Enter in either library search keeps the current hits as a durable tab. Local: a scratch list tab (persists and plays) resolved from the complete album+track match set. ADR-0164 opens library/sidebar and standalone database searches from cached tags and technicals without file discovery, with independent album-cover loading. Explicit Edit tags/ReplayGain operations capture revisionless physical cache rows on a worker (ADR-0165); grouped scroll ranges include every album header. MPD: a query-keyed, session-only snapshot tab in the ADR-0129 pattern; recommitting a query refreshes it in place, rows feed the live queue. Album-hit expansion inside MPD tabs remains open. |
| Track-view layouts | Partial | Both authorities share grouped side/header artwork, plain/compact presentations, and persisted semantic-column order, visibility, and widths. Arbitrary expression-defined columns/grouping remain open. |
| Workspace arrangement | Implemented | Versioned side-by-side, stacked, and tabbed panel composition with reorder/reset and validated restoration (ADRs 0026–0027). |
| Keyboard navigation | Implemented | Settings → Shortcuts edits and persists main-window bindings, detects duplicate/prefix conflicts, and restores defaults with Save/Cancel. Playback, Up Next, Follow playback, Jump to playing, file, search, and tab actions have defaults. Tab navigation, Home/End, selection keys, and keyboard context actions retain native behavior. |
| Command palette and shortcut editor | Implemented | Workspace → Commands… (`Ctrl+Shift+P`) searches a curated task inventory, hides unavailable commands, shows right-aligned shortcuts, and runs the existing action. Device names, rating values, and other parameter choices are excluded (ADR-0205). Settings → Shortcuts owns binding changes, conflict validation, and default restoration. |

Evidence: [workspace specification](ui-workspace.md),
[workspace actions](../src/bench/bench_workspace_layout.cpp),
[list actions](../src/bench/bench_list_tabs.cpp),
[track layouts](../src/uicommon/track_view_layout.hpp),
and [MPD retirement](adr/0224-retire-the-mpd-backend.md).

## Engines, libraries, and playback

The engine era (ADRs 0220–0236). Each engine owns its library, lists, queue,
playback, history and scrobbling; Trackknife, the Android app and `melody-cli`
are clients of it over protocol v1. The MPD client and everything built on it
-- MPD profiles, the live MPD queue, stored MPD playlists, server list tabs,
server-side search translation, MPD/local mapping -- were removed in
ADR-0224.

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Engine (`melodyd`) | Implemented | One process owns catalogue, lists and playback and keeps playing with every client closed (ADR-0220). Trackknife starts this computer's engine itself (ADR-0226); a headless engine runs anywhere. Stable engine id kept in its state directory (ADR-0234). See [melody.md](melody.md). |
| Protocol v1 | Implemented | JSON lines over a unix socket or TCP (ADR-0222); every TCP peer gives a password, no built-in TLS (ADR-0223). Playback states carry an engine-made `sequence`, and the volume or place an output was told shows until it catches up (ADR-0236). |
| Several engines at once | Implemented | Trackknife holds a connection per engine listed in Settings → Engine, each with its own library tab, tab group and lists; engines added or removed apply at once (ADR-0234). One engine plays at a time. A list mixing engines is not planned. |
| Output agents | Implemented | An engine plays on its own speakers or on agents -- another computer, a Pi, a phone -- and moves the music between them where it is (ADR-0228). Engines find each other and lend their speakers to every engine listed; the newest to start playing gets them (ADR-0229, ADR-0234). |
| Lists in the engine | Implemented, bounded | Named and working lists live in their engine and every client sees them (ADR-0233), with sync, conflicts, `list.relocate` after a move, and the Lists panel. Trackknife still keeps its own copy (step 3d open). |
| Android client | Implemented | Remote control, library, lists of every engine listed, Up Next; the phone as a speaker for every engine listed, sharing its one player; Opus over mobile data and offline albums (ADRs 0230, 0231, 0234). Its player follows the engine that plays. |
| `melody-cli` | Implemented | Transport, queue, search (`find` with tkq and `--format`), `current` with tkfmt formats, `--json`, meaningful exit codes. |
| `melody-watch` | Implemented | Tells an engine about file changes on a NAS it cannot watch itself (ADR-0232). An engine watching its own folders is open. |
| Up Next request queue | Implemented, bounded | Temporary FIFO with automatic return, held by the engine that plays; a flat panel with reorder/remove/clear/Undo/return-now and multi-selection edits (ADRs 0196, 0201). See [the guide](up-next.md). |
| Last.fm scrobbling and Love/Unlove | Implemented, bounded | Done by the engine that plays, so it works with Trackknife closed; the account is connected once in Trackknife and handed to each engine. See [guide](lastfm.md). |
| Local folders and intake | Implemented | Bookmarks, asynchronous filesystem browsing, file/folder/drop/CLI intake, lossless raw-path handling, and bounded CUE/chapter/subsong expansion into lists. |
| Engine libraries | Implemented | Folders are chosen per engine; **only Refresh starts a filesystem scan**. Paged artist/album/track browsing and search, offline retention, numbered tracks, covers, album counts before expansion, locate artist/album. Tag and move commits update the index without rescanning (ADRs 0115–0118, 0126, 0151). |
| Advanced library views | Partial | Structured `tkq-1` filters over every tag and technical value (ADR-0150), saved searches (ADR-0163), committed result tabs. Autoplaylists, custom tree expressions, logical-title indexing and a Trackknife artwork grid remain open. |
| Playback | Implemented | FFmpeg/libopenmpt sources through PipeWire, sample-range seeking, gapless transitions, buffer profiles, ReplayGain preamps, output/default/hotplug handling, all in the engine. |
| Playback modes | Implemented | Repeat, Random, Single/Consume including one-shot modes, owned by the engine (ADRs 0119–0121). |
| Track and album ratings | Implemented | 0–10 scale (ADR-0179), stored in the engine. Optionally written into files as `FMPS_RATING` and MP3 POPM (ADR-0237), with a plain 0–10 copy in a backup tag of the user's naming (ADR-0245). |
| Album shuffle, history, and resume | Implemented, bounded | Listening statistics, paused resume including Up Next, one-shot and continuous album shuffle, `HISTORY(...)` predicates and history sorting (ADRs 0207–0216), per engine. Calendar-relative time operators remain open. |
| MPRIS/media keys/notifications | Implemented | MPRIS and media keys control the engine that plays (ADR-0135); opt-in track-change notifications (ADRs 0144, 0195). The playing album's cover reaches both, as an `mpris:artUrl` and a notification image, from a file written once per album into the cache. LoopStatus and Shuffle map to the modes both ways: repeat and single loop the track, repeat the list, random or album random shuffle. |

Evidence: [unified engine](unified-engine.md), [melody.md](melody.md),
[local library](local-library.md), [playback](playback-library-conversion.md),
and [transport](../src/bench/bench_transport.cpp).

## Metadata, artwork, and file operations

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Tag editor draft workspace | Implemented | **Edit tags…** in Edit/local-track menus (Alt+Return) opens the Tags workspace. Full source paths in the file selector display valid Unicode normally and escape invalid bytes/control characters (covered by UI model regression tests); one file view per editor, hosted in the Files sidebar with editor-owned selection (ADR-0194), and checkbox scope indicators with neutral padded rows, mouse/Space toggling, and tabbed file-selection-driven Fields/Original/Draft editing, common/mixed/missing/partial states, arbitrary ordered multi-values, exact-native field identity, fuzzy field completion, provenance, and draft undo/redo. A read-only technical summary (codec, rate, depth, channels, bitrate, summed duration) probes the selected files in the background (ADR-0152). ADR-0157 adds field-name/changed-only filtering, selection-preserving file-pane collapse, visible change counts, and non-color edited-field markers. ADR-0177 retires ADR-0170's ambiguous field-layout UI: Properties shows the complete inventory and specialized Suggest lives under More. Previewed field allowlist/blocklist cleanup belongs in Scripts. |
| Tagging scripts | Implemented | Versioned saved typed transformations, exact-value cleanup, conditional removal, capture patterns, ordinary and grouped numbering with totals, previewed field allowlist/blocklist cleanup, and native JSON interchange. A step converts another player's rating, on the scale it was kept on, into `FMPS_RATING` (ADR-0244). The Raw script tab is the whole script as text, one `$`-statement per step with full `tkfmt-1` (ADR-0241, [tagging-scripts.md](tagging-scripts.md)). The bounded Picard paste importer is not general Picard compatibility (ADRs 0065–0072, 0104, 0178). |
| Automatic scripts and Apply | Implemented | Automatic chains stage visible undoable edits. Apply writes exactly the staged draft; scripts do not run invisibly at write time (ADR-0093). |
| Metadata providers | Implemented | Typed observation-only proposals, provenance/confidence, validated staging, and selection-consistency Suggest. Public provider/plugin APIs remain deferred. |
| MusicBrainz and AcoustID | Implemented | Text search, ranked release versions, matching, identifier/credit/sort metadata proposals, and optional fingerprint identification through external `fpcalc`. Cache-first paced requests and typed failures are implemented (ADRs 0088–0096). ADR-0158 removes exact track-count search filtering and retries throttled requests with bounded Retry-After-aware cooldown. ADR-0162 presents aligned local-filename and MusicBrainz panes with drag/button reordering, fixed album tracks, visible pairing markers, linked selection/scrolling, gaps, and unmatched files, including untagged albums. |
| Cover Art Archive | Implemented | Fetch front cover from one unambiguous draft-or-baseline release ID, or choose archive images by role. Embedding uses the qualified artwork path. Same-workspace cover commits preserve pending tag drafts and undo history (ADRs 0091, 0094, 0121). |
| Text metadata writes | Partial | Qualified FLAC, WavPack, MP3, Vorbis, Opus, and MP4/M4A adapters (ADR-0136), with preservation checks and journaled publication. Other containers remain read-only for text mutation; see the format table below. |
| Artwork management | Partial | Qualified embedded inventory and add/replace/remove/copy for native FLAC, ID3v2 APIC, and MP4 covr (ADR-0137; covr entries are untyped front covers), plus thumbnails and bounded export. External PNG/JPEG files are donors/export sources; Ogg and other containers lack qualified artwork mutation. ADRs 0159–0160 add staged multi-picture removal/replacement/addition, unified tag/artwork Apply, Discard, same-role cover handling, and asynchronous Before/After previews of the pending draft. All changes to each file share one verified publication and recovery backup; real FLAC/MP3/MP4 tests cover failure atomicity and whole-file undo. ADR-0161 adds explicit retry of unfinished physical tag/artwork files with revision and recovery guards. ADR-0184 adds the Fields front-cover thumbnail, drop/paste/fetch staging, captured embed/folder policy, explicit destination review, and schema-38 journaled folder create/replace/recovery. Folder-only front additions do not require an embedded writer. Folder covers must be saved before Rename/Move. External image deletion remains open. The inventory is bounded to 64 physical sources. |
| Rename/move and combined preparation | Partial | Reusable naming layouts/destinations, versioned `linux-v1` and `portable-v1` sanitization, fresh conflict checks, same/cross-filesystem publication, and verified dependent list/cache/library/playback relocation. Combined native-FLAC tag/path publication is qualified. User-defined sanitization and Unicode-normalization policies remain open. |
| Recovery and commit feedback | Implemented | Tag and folder-cover saves work on filesystems without hard links (SMB, FAT/exFAT, some FUSE): the backup is then a verified copy whose identity is journaled before publication, and a folder cover is replaced by plain rename where directory-entry exchange is refused (ADR-0248). Where renames give files new inode numbers (the macOS SMB client), checks after Trackknife's own renames compare all but the inode, asked of the filesystem once per device (ADR-0249). ADR-0193 scopes save admission to the target raw path: damaged unrelated journals do not block media or folder-cover saves, while same-source evidence remains mandatory. Journals and automatic recovery remain. Unresolved incidents surface once; ordinary Apply uses inline progress and problems-only feedback. The old history/undo window and cross-restart undo retention were removed in ADR-0084. Do not confuse this with tag-draft undo or local removal/reorder undo. |

Evidence: [metadata and files](metadata-and-files.md),
[writer capabilities](../src/metadata/src/local_reader.cpp),
[WYSIWYG Apply](adr/0093-wysiwyg-apply-and-staged-automatic-scripts.md),
[grouped numbering](adr/0104-grouped-numbering-transformation.md), and
[recovery/undo decision](adr/0084-silent-recovery-and-draft-color-semantics.md).

## ReplayGain

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Measurement core | Implemented | libebur128 integrated loudness, sample and oversampled true peak, correct album programme reduction, high-rate native analysis, and bounded parallel decode with source-revision checks (ADRs 0097–0099). An opt-in policy proposes the true peak in `REPLAYGAIN_*_PEAK`; sidecars and the CSV export record the peak kind (ADR-0148). |
| Grouping and Properties scan | Implemented | Track, selection-as-album, release-aware, disc-merging (ADR-0146 strips trailing disc designators), and `tkfmt-1` grouping; progress/cancel, incomplete-album feedback, visible ReplayGain draft proposals, a "Retry N failed" link that re-runs only failed or cancelled measurements, CSV export of the measurement snapshot, and a per-track "Loudness sources…" provenance view (ADR-0147). A compact scan-and-write dialog sits in the track context menu, sharing options, scan engine, and the journaled write pipeline (ADR-0156). Storage coverage remains separate. |
| ReplayGain dialog presentation | Implemented | ADR-0219 adds clearer scan modes, a worker-prepared read-only group preview, grouped storage/peak options, phase-labelled progress and Stop, plus explicit scan-and-write wording distinguishing tags from permanent PCM processing. Local and mapped server selections share the same UI and journaled writer; real-file regressions cover both. |
| CUE/chapter/subsong scan from Properties | Implemented | ADR-0124 preserves decoder selections and exact sample ranges through capture and subset rescans. Real-file workspace regressions compare gains, peaks, and album gains with direct logical-source scans. Logical-track loudness never becomes whole-file tags; CUE tracks persist into the sheet (ADR-0139), chapters/subsongs/segments into the loudness sidecar (ADR-0141). |
| Loudness sidecar | Implemented | ADR-0141: `<file>.tkmeta` beside the source — versioned strict JSON, entries keyed by decoder selection + sample range, staleness by audio size+mtime (survives timestamp-preserving copies). Write plan routes non-CUE logical ReplayGain there (ADR-0143 adds unwritable whole-file formats); merges over an existing sidecar run the full journal lifecycle with retained undoable pre-images and crash recovery, emptied sidecars publish as empty documents, creation stays a direct atomic publish (ADR-0145). Probing projects fresh sidecars onto rows at sidecar provenance. |
| CUE sheet ReplayGain carriage | Implemented | ADR-0139: scan results for CUE logical tracks route through the write plan into foobar2000-convention `REM REPLAYGAIN_*` lines — per-track in the TRACK block, album values in the header with cross-track agreement enforced — published by a revision-gated atomic rewrite whose preservation proof keeps every other byte. ADR-0145 journals every sheet rewrite: crash recovery verifies REM content against the record's evidence, and the byte pre-image is retained and undoable. |
| Embedded storage and fallback | Implemented | Conventional ReplayGain values use the ordinary qualified text-write pipeline; CUE tracks use the sheet (ADR-0139), other logical tracks the sidecar (ADR-0141), whole-file tracks in unwritable formats divert to the sidecar automatically (ADR-0143), and a persisted "Store in sidecar only" policy diverts every non-CUE source regardless of writability (ADR-0146). Opus sources stage RFC 7845 R128 Q7.8 comments through the same qualified Ogg writer (ADR-0149). The all-fixture scan gate remains independent of tag writability (ADR-0172). |
| Local playback gain | Implemented | Off/Track/Album/Automatic, album-to-track fallback, matching stored-peak clipping prevention, exact bypass, and separate persisted ±20 dB preamps for tracks with and without loudness data (ADR-0138). The explicit override consumes fresh sidecar values first, then CUE sheet REM values, then the decoder's tags — at load and across gapless takeovers (ADRs 0139/0141). Opus R128 comments are read per RFC 7845 and lifted onto the ReplayGain scale (ADR-0149). ADR-0172 fixes this as the M7 processing policy; additional DSP preference variants remain optional expansion. |

Evidence: [ReplayGain specification](replaygain.md),
[scan core](../src/loudness/include/trackknife/loudness/scan.hpp),
[Properties scan](../src/bench/metadata_properties_dialog.cpp), and
[decoder gain metadata](../src/formats/src/decoder.cpp).

## Conversion and organized output

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Encoder presets | Implemented | Runtime availability checks, built-in FLAC/Opus/MP3/Vorbis presets, and saved user presets constrained to qualified targets (ADRs 0105, 0108). A saved preset also snapshots destination/naming, processing, mapping, concurrency, and exact FFmpeg backend versions (ADR-0173). |
| Naming and destinations | Implemented | Settings separates layout and destination editors into tabs with compact preset selectors and full-width fields (ADR-0185 presentation update). Explicit destination roots, `tkfmt-1` path preview, saved naming layouts/destinations, one output per exact logical track, and deterministic full-source-path mirroring relative to `/` (ADRs 0132/0154). Grouped chapter containers and merge-all output require a separate metadata/boundary contract and are optional expansion (ADR-0173). |
| Signal processing | Implemented | Forced or downsample-only rate policy, keep/16/24-bit policy, high-passed triangular dither for 16-bit quantization, Keep/Mono/Stereo channel policy, and optional permanent Track/Album ReplayGain with fallback and peak protection (ADRs 0134/0173). ADR-0218 makes permanent gain opt-in per dialog/preset selection, ignores old saved gain settings, and adds an inline warning plus Cancel-default confirmation; real-file UI tests cover defaults, cancellation and explicit PCM processing. A general ordered DSP graph remains optional expansion. |
| Text metadata transfer | Implemented | Mux-time text mappings with exact reread verification for qualified outputs. This does not include proof of interoperable loudness semantics in every container. |
| Artwork and output loudness | Implemented | One resolved cover per source is embedded into every qualified preset and reread byte-exactly before publication (ADR-0131). Conversion strips stale `REPLAYGAIN_*`/`R128_*` fields and verifies none survive (ADR-0133), including after permanent gain. Multiple pictures, missing-cover reports, and automatic output rescanning remain optional expansion. |
| Parallel conversion and verification | Implemented | Bounded workers, progress/cancellation, per-item failures, source-revision checks, hidden temporary outputs, full decode/duration/format verification, and no-overwrite publication. General retry/resource scheduling remains open. |
| Limited filesystems | Partial | Publication and locking fallbacks plus tested NFS paths (ADR-0111). Artwork undo on exchange-less filesystems remains a core follow-up, not an exposed undo feature. |

Evidence: [conversion](playback-library-conversion.md#converter),
[conversion request](../src/convert/include/trackknife/convert/convert.hpp),
[conversion implementation](../src/convert/src/convert.cpp), and ADRs 0105–0111.

## Foundations and later work

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| `tkfmt-1` | Implemented | Versioned deterministic pure formatting, corpus, parser/evaluator, and shared typed use. Core support does not imply every UI has an expression editor; no foobar2000/Picard script compatibility promise. Help → tkfmt-1 reference opens the shipped specification in the browser (ADR-0242). `$decimal` gives fractions, such as rating scales (ADR-0243). |
| Persistence | Implemented | Schema **48**, with reversible migrations for profiles, lists, layouts, scripts, presets, caches, library records, and operation journals. Serialized worker ownership and durable list flush. ADR-0175 adds a File-menu online database backup with integrity/schema validation and a versioned settings companion; restart-safe restore retains the previous database as a rollback. |
| Job execution | Partial | Bounded operation-specific pools with cancellation/progress and partial results. Shared resource-class scheduling and generalized retry remain open. |
| Performance and hardening | Partial | Regression suites, sanitizer/static-analysis tooling, fuzz targets, and cached-view benchmarks exist. Representative large-library/network/device latency and sustained combined-workflow validation remain release work. Historical test counts are not current suite results. |
| Packaging and releases | Implemented | The native Arch PKGBUILD/desktop entry, CI sanitizer/fuzz/static-analysis gates, dependency and Qt LGPL inventory, workspace backup/restore, capability diagnostics, preset export, accessibility automation, M10 validation record, and per-artifact release checklist are present. Flatpak is not claimed. |
| Collection integrity/comparison/relinking | Open | Internal conversion verification exists; standalone collection scans, audio duplicate comparison, missing-file relinking, and album-completeness tools do not. |
| Melody playback endpoint | Implemented | ADR-0174 adds a capability-gated v2 endpoint backed by the shared local player: registration, version-consistent queue sync, direct or ID-streamed sources, ReplayGain, gapless preload, clock/advance reports, and reconnect. Locale-independent outgoing clock reports preserve fractional seek positions during output handoff. Host-qualified output names prevent clients on different computers from replacing each other; registration and audio fixtures cover the default name and music-root mappings with trailing separators. Stock MPD remains client-only. |
| DSP graph | Deferred | Versioned processing presets and exact bypass require separate qualification. |
| Dynamic playlists | Implemented, bounded | One engine/editor for local and MPD/Melody libraries: profile-scoped saved `tkq-1` rules, live rule refresh while open, limits/shuffle, and explicit Last.fm similar/loved/top/tag sources matched to library tracks. Homogeneous typed results, cancellation, and snapshot/new-tab or server queue-add actions. Dynamic results and snapshots use persistent single-line layouts without album grouping. Stock MPD supports compatible tag filters; Melody adds rating/technical filters. Provider matching is exact artist/title, up to 500 candidates independent of playlist length. Last.fm refreshes randomly select matches with minimum overlap against the previous result, remembered per definition/library across restarts; queue replenishment and automatic stored-playlist synchronization remain open. See [guide](dynamic-playlists.md) and ADR-0195. |
| Dynamic result interactions | Implemented, bounded | ADR-0216 adds row/multi-selection, Enter/double-click playback via stable snapshots, source-aware tools/ratings/navigation/Up Next, copy-only outward drag, explicit editable snapshots, and source/occurrence-based refresh anchors. Tests cover both result models, selection retention, numeric history queries, local action targeting, snapshot isolation and protocol duplicates. Persistent live autoplaylist tabs remain open. |
| Grouped search presets | Implemented | ADR-0217: 25 parameterized/fixed starting points under Explore, Favourites, Listening, Audio properties and Library maintenance. Generated tkq-1 stays editable and saveable. Local and Melody use the same catalog, with unsupported remote presets hidden using execution capability checks. Corpus and UI tests cover generation, escaping, bounds, translation, scope gates, cancellation and input focus. Recently-added/lossless classification and a general visual builder remain deferred. |
| Query dialect | Implemented | `tkq-1` (ADR-0150): own versioned dialect, foobar-inspired keyword surface, normative spec in [query-language.md](query-language.md); no external dialect compatibility. Structured filtering runs over the migration-30 index substrate behind the library panel's query toggle and the standalone search dialog (ADR-0153), whose tab scope shares the exact row evaluation and probes missing technicals on demand. Saved searches with explicit dialects are implemented in ADR-0163; shared dynamic definitions are covered by ADR-0195. A visual query builder remains open. |
| Plugin API, CD ripping, radio, remote import | Deferred | Separate product/compatibility decisions. No transactional Melody upload/import capability is claimed by this client. |

## Format-support dimensions

Playback, text writing, artwork writing, ReplayGain storage, encoding, and
integrity verification are independent capabilities. FFmpeg availability alone
never qualifies a writer or proves exact seek/gapless behavior.

| Container | Qualified text mutation | Qualified artwork mutation | Qualified conversion output |
| --- | --- | --- | --- |
| Native FLAC | Yes | Yes | Yes |
| Native WavPack | Yes, within adapter restrictions | No | No |
| MP3 | Yes, qualified ID3 path | Yes, APIC qualified path (ADR-0137) | Yes |
| Ogg Vorbis | Yes, single-stream qualified path | No | Yes |
| Ogg Opus | Yes, single-stream qualified path | No | Yes |
| MP4/M4A (AAC/ALAC) | Yes, box-preserving qualified path (ADR-0136) | Yes, untyped covr qualified path (ADR-0137) | No |
| WAV/RF64/Wave64, AIFF, other decoded containers | No qualified writer | No | No qualified output preset |

Conversion outputs remain conditional on the installed FFmpeg encoder/muxer.
WavPack trailers and chained/multiplexed Ogg streams have explicit restrictions;
see [ADR-0095](adr/0095-prepared-copy-wavpack-text-writer.md) and
[ADR-0114](adr/0114-qualified-ogg-writers.md). Text mutation qualification does
not automatically qualify ReplayGain interoperability or logical-track writes.
Use the real fixtures and feature-specific specifications for exact claims.

Playback tabs distinguish browsing from playback: the selected tab has a lighter
background and accent underline; an accent speaker icon identifies each active
playback queue, including while paused or stopped. Titles use normal text color.

Mapped server Edit tags opens the standard editor tab with its sidebar file list,
without an intermediate local queue tab (ADR-0203). Real-file metadata and revisions are captured
asynchronously on the client; server-side reads/writes remain proposed. The
mapped-action UI regression covers metadata capture and invalid/missing paths.

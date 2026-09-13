# Feature matrix

Last reconciled: 2026-09-06 against source baseline `bfc99e3` and accepted ADRs
through ADR-0128. This matrix describes the current primary workspace, not the
retired MPD shell. The product is Trackbench; the current executable is named
`trackknife` (`src/bench`). MPD and local queues retain separate authorities.

Use the [roadmap](roadmap.md) for implementation priorities and
[MILESTONES.md](../MILESTONES.md) for capability gates and historical evidence.
M5 remains the active acceptance gate; implementation has also landed in M6–M8.
An implemented feature does not by itself close a milestone.

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

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Unified MPD/local workspace | Implemented | Authority-bound tabs switch transport, sources, outputs, and available commands. Local tagging/filesystem operations cannot target MPD rows. The old standalone MPD shell is removed; no shell/profile migration remains (ADRs 0058, 0071). |
| Local working-list tabs | Implemented | Persistent scratch/named lists, rename, save, pin, duplicate, reorder, dirty-close protection, row removal, and cross-list copy/move and drag/drop. Duplicate occurrences and raw paths survive persistence. |
| Local list editing tools | Partial | Per-tab removal/reorder undo/redo with shortcuts, menus, bounded session history, and playback-preserving index updates is implemented (ADR-0123). ADR-0127 adds cancellable whole-list natural sorting with tkfmt-1 presets/custom expressions, reversal, and logical-source-aware keep-first duplicate removal, each with named undo/redo. Add/replace/cross-tab history remains open. Tag-draft undo remains separate. See [roadmap 1](roadmap.md#1-queue-and-playlist-editing). |
| Find within track lists | Implemented | ADR-0125 adds Ctrl+F, next/previous matches, wraparound, progress/cancellation, and Escape to close in local lists and the MPD queue. ADR-0142 widens the haystack to every metadata value at every provenance, formatted durations, MPD `audio_format`/unknown pairs, plus the escaped paths/server URIs — still bounded worker batches without filtering, playback changes, or server commands. Local codec/bit-rate/sample-rate search awaits probe-technicals retention. |
| Portable playlists | Partial | ADR-0128 adds cancellable M3U8 import into a new local list and whole-local-list export to a new file. Relative paths, duplicate/offline occurrences, EXTINF labels/durations, and escaped raw filenames are preserved. Logical selections and remote references fail explicitly; existing files are never replaced. M3U encoding policy, XSPF/PLS, and replacement workflows remain open. |
| MPD stored playlists | Implemented | ADR-0129: sidebar Playlists list, one closable server-keyed tab per playlist, capability-gated add/remove/reorder/load/clear/rename/delete as server round trips, batched `playlistadd`, and `stored_playlist` idle refresh. Multi-row reorder, tab persistence across restart, and drops from live search results remain open. |
| Committed search-result tabs | Implemented | ADR-0140: Enter in either library search keeps the current hits as a durable tab. Local: a full-citizen scratch list tab (persists, probes, plays) resolved from the complete album+track match set. MPD: a query-keyed, session-only snapshot tab in the ADR-0129 pattern; recommitting a query refreshes it in place, rows feed the live queue. Album-hit expansion inside MPD tabs remains open. |
| Track-view layouts | Partial | Both authorities share grouped side/header artwork, plain/compact presentations, and persisted semantic-column order, visibility, and widths. Arbitrary expression-defined columns/grouping remain open. |
| Workspace arrangement | Implemented | Versioned side-by-side, stacked, and tabbed panel composition with reorder/reset and validated restoration (ADRs 0026–0027). |
| Keyboard navigation | Implemented | Fixed action shortcuts, tab navigation, Home/End and Shift boundary selection, and keyboard context actions. |
| Command palette and shortcut editor | Backend only | A reusable command-palette widget exists, but the primary workspace does not expose it or a configurable shortcut editor. Older shell completion claims do not apply. |

Evidence: [workspace specification](ui-workspace.md),
[workspace actions](../src/bench/bench_workspace_layout.cpp),
[list actions](../src/bench/bench_list_tabs.cpp),
[track layouts](../src/uicommon/track_view_layout.hpp),
[MPD controller](../src/quick/mpd_probe_controller.hpp), and
[shell retirement](adr/0071-retire-trackknife-compatibility-shell.md).

## Libraries, MPD, and playback

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| MPD session and profiles | Implemented | Saved profiles, TCP/Unix socket connections, session-only passwords, capabilities, idle updates, bounded reconnect, and authoritative reconciliation. Secure password storage/authenticated auto-connect remains deferred. |
| MPD transport and outputs | Implemented | Play/pause/stop/seek/previous/next, volume, playback modes, advertised ReplayGain, and stock additive outputs. Melody output status/exclusive selection are capability-gated. This is client control, not a Melody playback endpoint. |
| Live MPD queue | Implemented | Stable-ID add/insert/remove/reorder, crop, priority controls and badges, duplicate occurrences, context actions, and library-to-queue drops. Conflicts refresh authoritative state without replaying ambiguous mutations. |
| MPD browse and search | Implemented | Artist/album browsing, numbered tracks and covers, A–Z/Latest ordering, Go to artist/album, bounded library-integrated search and continuation, and append/next/replace actions. Search shares the local Albums/Tracks tree, row renderer, 200 ms debounce, one-character queries, expansion, multi-selection actions, and queue drops; it survives focus and authority changes (ADRs 0120, 0122). Browse resets also refresh retained search covers and reject obsolete artwork responses (ADR-0126). A user-facing custom tree editor remains open. |
| MPD/local mapping | Implemented | Configured music-root mapping enables explicit Load as local files. Update this folder in MPD is a separate explicit server action. Local publication never implicitly updates MPD (ADRs 0112–0113). |
| Local folders and intake | Implemented | Bookmarks, asynchronous filesystem browsing, file/folder/drop/CLI intake, lossless raw-path handling, and bounded CUE/chapter/subsong expansion into local lists. |
| Optional local library | Implemented | Chosen roots, incremental revision checks, paged artist/album/track browsing and search, offline retention, numbered tracks, covers, context actions, and local-only drops. **Only Refresh starts a filesystem scan.** Scan preparation runs on a bounded worker pool with serial guarded commits (ADR-0151). Tag/move commits update the cached index transactionally without rescanning (ADRs 0115–0118). Complete scans prune confirmed deleted entries while preserving offline/uncertain paths and working lists (ADR-0126). |
| Advanced local library views | Partial | Structured tkq filters over every indexed tag and probed technicals (ADR-0150), with committed query tabs through the ADR-0140 path. Saved searches/autoplaylists, custom tree expressions, logical-title indexing, and an artwork grid remain open. See [roadmap 2](roadmap.md#2-library-filters-and-saved-searches). |
| Local playback | Implemented | FFmpeg/libopenmpt sources through PipeWire, sample-range seeking, qualified gapless transitions, originating-list progression, volume, buffer profiles, and output/default/hotplug handling. Hardware-format coverage remains bounded by qualification. |
| Local playback modes | Implemented | Repeat, track Random, Single/Consume including one-shot modes, and persistent local settings. Background progression continues in its original list. Metadata/path publication preserves row identity and advancement (ADRs 0119–0121). |
| Album shuffle, history, and resume | Open | Album shuffle preserving track order, play counts, last played, ratings, and persisted playback-position restoration. See [roadmap 7](roadmap.md#7-listening-history-and-album-oriented-playback). |
| MPRIS/media keys/notifications | Partial | `org.mpris.MediaPlayer2.trackknife` exposes the active authority's transport, typed metadata, position/Seeked, and volume, and routes desktop commands (including media keys) through the same transport actions as the visible controls (ADR-0135). ADR-0144 adds opt-in quiet track-change notifications (background-only, one replaced transient bubble, silent degradation) fed by the same now-playing snapshot. Notification artwork, MPRIS artwork URLs, and LoopStatus/Shuffle mapping remain open. |

Evidence: [MPD](mpd-client.md), [local library](local-library.md),
[playback](playback-library-conversion.md), [transport](../src/bench/bench_transport.cpp),
and [local playback modes](adr/0119-local-playback-modes-and-replaygain.md).

## Metadata, artwork, and file operations

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Tag editor draft workspace | Implemented | **Edit tags…** in Edit/local-track menus (Alt+Return) opens the Tags workspace. Tabbed file-selection-driven Fields/Original/Draft editing, common/mixed/missing/partial states, arbitrary ordered multi-values, exact-native field identity, fuzzy field completion, provenance, and draft undo/redo. A read-only technical summary (codec, rate, depth, channels, bitrate, summed duration) probes the selected files in the background (ADR-0152). ADR-0157 adds field-name/changed-only filtering, selection-preserving file-pane collapse, visible change counts, and non-color edited-field markers. Saved field-layout presets remain open. |
| Tagging scripts | Implemented | Versioned saved typed transformations, exact-value cleanup, conditional removal, capture patterns, ordinary and grouped numbering with totals, and native JSON interchange. The bounded raw-script importer is not general Picard compatibility (ADRs 0065–0072, 0104). |
| Automatic scripts and Apply | Implemented | Automatic chains stage visible undoable edits. Apply writes exactly the staged draft; scripts do not run invisibly at write time (ADR-0093). |
| Metadata providers | Implemented | Typed observation-only proposals, provenance/confidence, validated staging, and selection-consistency Suggest. Public provider/plugin APIs remain deferred. |
| MusicBrainz and AcoustID | Implemented | Text search, ranked release versions, matching, identifier/credit/sort metadata proposals, and optional fingerprint identification through external `fpcalc`. Cache-first paced requests and typed failures are implemented (ADRs 0088–0096). ADR-0158 removes exact track-count search filtering, adds explicit side-by-side file/track assignment before staging (including untagged files), and retries throttled requests with bounded Retry-After-aware cooldown. |
| Cover Art Archive | Implemented | Fetch front cover from one unambiguous draft-or-baseline release ID, or choose archive images by role. Embedding uses the qualified artwork path. Same-workspace cover commits preserve pending tag drafts and undo history (ADRs 0091, 0094, 0121). |
| Text metadata writes | Partial | Qualified FLAC, WavPack, MP3, Vorbis, Opus, and MP4/M4A adapters (ADR-0136), with preservation checks and journaled publication. Other containers remain read-only for text mutation; see the format table below. |
| Artwork management | Partial | Qualified embedded inventory and add/replace/remove/copy for native FLAC, ID3v2 APIC, and MP4 covr (ADR-0137; covr entries are untyped front covers), plus thumbnails and bounded export. External PNG/JPEG files are donors/export sources; Ogg and other containers lack qualified artwork mutation. ADR-0159 adds staged multi-picture removal/replacement/addition, explicit Save/Discard, and same-role cover handling. Changes serialize per file with per-picture recovery; external image deletion and atomic multi-picture publication remain open. The inventory is bounded to 64 physical sources. |
| Rename/move and combined preparation | Partial | Reusable naming layouts/destinations, `linux-v1` sanitization, fresh conflict checks, same/cross-filesystem publication, and verified dependent list/cache/library/playback relocation. Combined native-FLAC tag/path publication is qualified. Portable/custom sanitization and Unicode-normalization policies remain open. |
| Recovery and commit feedback | Implemented | Journals and automatic recovery remain. Unresolved incidents surface once; ordinary Apply uses inline progress and problems-only feedback. The old history/undo window and cross-restart undo retention were removed in ADR-0084. Do not confuse this with tag-draft undo or local removal/reorder undo. |

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
| CUE/chapter/subsong scan from Properties | Implemented | ADR-0124 preserves decoder selections and exact sample ranges through capture and subset rescans. Real-file workspace regressions compare gains, peaks, and album gains with direct logical-source scans. Logical-track loudness never becomes whole-file tags; CUE tracks persist into the sheet (ADR-0139), chapters/subsongs/segments into the loudness sidecar (ADR-0141). |
| Loudness sidecar | Implemented | ADR-0141: `<file>.tkmeta` beside the source — versioned strict JSON, entries keyed by decoder selection + sample range, staleness by audio size+mtime (survives timestamp-preserving copies). Write plan routes non-CUE logical ReplayGain there (ADR-0143 adds unwritable whole-file formats); merges over an existing sidecar run the full journal lifecycle with retained undoable pre-images and crash recovery, emptied sidecars publish as empty documents, creation stays a direct atomic publish (ADR-0145). Probing projects fresh sidecars onto rows at sidecar provenance. |
| CUE sheet ReplayGain carriage | Implemented | ADR-0139: scan results for CUE logical tracks route through the write plan into foobar2000-convention `REM REPLAYGAIN_*` lines — per-track in the TRACK block, album values in the header with cross-track agreement enforced — published by a revision-gated atomic rewrite whose preservation proof keeps every other byte. ADR-0145 journals every sheet rewrite: crash recovery verifies REM content against the record's evidence, and the byte pre-image is retained and undoable. |
| Embedded storage and fallback | Partial | Conventional ReplayGain values use the ordinary qualified text-write pipeline; CUE tracks use the sheet (ADR-0139), other logical tracks the sidecar (ADR-0141), whole-file tracks in unwritable formats divert to the sidecar automatically (ADR-0143), and a persisted "Store in sidecar only" policy diverts every non-CUE source regardless of writability (ADR-0146). Opus sources stage RFC 7845 R128 Q7.8 comments through the same qualified Ogg writer (ADR-0149). Scanning stays independent of tag writability. |
| Local playback gain | Partial | Off/Track/Album/Automatic, album-to-track fallback, matching-peak clipping prevention, and separate persisted ±20 dB preamps for tracks with and without loudness data (ADR-0138). The explicit override consumes fresh sidecar values first, then CUE sheet REM values, then the decoder's tags — at load and across gapless takeovers (ADRs 0139/0141). Opus R128 comments are read per RFC 7845 and lifted onto the ReplayGain scale (ADR-0149). |

Evidence: [ReplayGain specification](replaygain.md),
[scan core](../src/loudness/include/trackknife/loudness/scan.hpp),
[Properties scan](../src/bench/metadata_properties_dialog.cpp), and
[decoder gain metadata](../src/formats/src/decoder.cpp).

## Conversion and organized output

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| Encoder presets | Implemented | Runtime availability checks, built-in FLAC/Opus/MP3/Vorbis presets, and saved user presets constrained to qualified targets (ADRs 0105, 0108). |
| Naming and destinations | Partial | Explicit destination roots, `tkfmt-1` path preview, saved naming layouts/destinations, one output per logical track, and a mirror-source-structure mode below the inferred common source root (ADR-0132; editable roots remain open). Grouped/merge output modes remain open. |
| Signal processing | Partial | Target resampling, 16/24-bit policy where applicable, dither when quantizing to 16-bit, downsample-only caps, and keep-source stored depth (ADR-0134). Channel policy, DSP, and permanent gain application remain open. |
| Text metadata transfer | Implemented | Mux-time text mappings with exact reread verification for qualified outputs. This does not include proof of interoperable loudness semantics in every container. |
| Artwork and output loudness | Partial | One resolved cover per source is embedded into every qualified preset and reread byte-exactly before publication (ADR-0131); multi-picture carriage and per-item "no usable artwork" reporting remain open. Conversion strips stale `REPLAYGAIN_*`/`R128_*` fields and verifies none survive (ADR-0133); automatically rescanning converted outputs remains open. |
| Parallel conversion and verification | Implemented | Bounded workers, progress/cancellation, per-item failures, source-revision checks, hidden temporary outputs, full decode/duration/format verification, and no-overwrite publication. General retry/resource scheduling remains open. |
| Limited filesystems | Partial | Publication and locking fallbacks plus tested NFS paths (ADR-0111). Artwork undo on exchange-less filesystems remains a core follow-up, not an exposed undo feature. |

Evidence: [conversion](playback-library-conversion.md#converter),
[conversion request](../src/convert/include/trackknife/convert/convert.hpp),
[conversion implementation](../src/convert/src/convert.cpp), and ADRs 0105–0111.

## Foundations and later work

| Capability | Status | Current behavior and remaining work |
| --- | --- | --- |
| `tkfmt-1` | Implemented | Versioned deterministic pure formatting, corpus, parser/evaluator, and shared typed use. Core support does not imply every UI has an expression editor; no foobar2000/Picard script compatibility promise. |
| Persistence | Implemented | Schema **28**, with reversible migrations for profiles, lists, layouts, scripts, presets, caches, library records, and operation journals. Serialized worker ownership and durable list flush; general user backup/restore remains open. |
| Job execution | Partial | Bounded operation-specific pools with cancellation/progress and partial results. Shared resource-class scheduling and generalized retry remain open. |
| Performance and hardening | Partial | Regression suites, sanitizer/static-analysis tooling, fuzz targets, and cached-view benchmarks exist. Representative large-library/network/device latency and sustained combined-workflow validation remain release work. Historical test counts are not current suite results. |
| Packaging and releases | Partial | Arch PKGBUILD/desktop entry and CI exist. Broader packaging/Flatpak, accessibility review, release documentation, backup/restore, and release acceptance remain M10 work. |
| Collection integrity/comparison/relinking | Open | Internal conversion verification exists; standalone collection scans, audio duplicate comparison, missing-file relinking, and album-completeness tools do not. |
| Melody playback endpoint | Open | Client output controls exist; registration, streaming/direct-source playback, queue/clock synchronization, preload, and reconnect as an endpoint do not. |
| DSP graph | Deferred | Versioned processing presets and exact bypass require separate qualification. |
| Query dialect | Implemented | `tkq-1` (ADR-0150): own versioned dialect, foobar-inspired keyword surface, normative spec in [query-language.md](query-language.md); no external dialect compatibility. Structured filtering runs over the migration-30 index substrate behind the library panel's query toggle and the standalone search dialog (ADR-0153), whose tab scope shares the exact row evaluation and probes missing technicals on demand. Saved searches, autoplaylists, and the query builder remain open. |
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

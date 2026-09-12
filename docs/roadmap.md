# Trackbench feature roadmap

Last reconciled: 2026-09-08 against source baseline `59e6965`.

**Proposal:** Prioritized open work, saved at the user's request. The
[feature matrix](feature-matrix.md) records what currently exists;
[MILESTONES.md](../MILESTONES.md) retains capability gates and historical
implementation evidence. M5 remains the active acceptance gate. This roadmap
neither reopens completed implementations nor claims new milestone completion.

Playlist usability, complete album conversion, and library organization are
the main priorities. The eight areas below retain the review's suggested
order; correctness issues in an affected workflow come before feature expansion.
Checkboxes record open and completed slices. Detailed product decisions need ADRs,
regressions, and a feature-matrix update before completion is recorded.

## Correctness prerequisite

- [x] Reproduce the Properties ReplayGain scan's handling of CUE/chapter ranges
  and subsong selections, then preserve that identity through scan requests
  (ADR-0124).

Real-file workspace regressions reproduced incorrect gains for all three
source types. Properties now preserves decoder selections and exact sample
ranges, including subset rescans. Logical-track measurements remain visible
drafts; whole-file embedded writes are blocked until an appropriate storage
target exists. See [ReplayGain](#5-universal-replaygain-support).

User-reported library issues (2026-09-06), addressed in ADR-0126:

- [x] Remove confirmed deleted subfolders after a complete explicit Refresh,
  including within an accessible network mount. Incomplete/cancelled scans and
  unavailable mounts retain cached entries; changed-device evidence prevents an
  empty mountpoint from being treated as a deletion. Cleanup leaves files and
  working lists intact.
- [x] Refresh search artwork with library browsing. The reproduced MPD defect
  left search thumbnails unchanged when database events reset the browse tree.
  Search now invalidates those images and obsolete requests while retaining its
  query and rows. Local real-file regressions also verify embedded/folder cover
  refresh and browse/search transitions without search-triggered scans.

References: [library refresh decision](adr/0126-library-deletion-and-search-artwork-refresh.md),
[local library](local-library.md).

## 1. Queue and playlist editing

**Proposal:** Make working lists easy to manage and portable between players.

- [x] Undo/redo for local list removal and rearrangement (ADR-0123; bounded,
  per-tab session history). Adding/replacing contents and atomic cross-tab
  history remain follow-up work.
- [x] Find within the current local list or MPD queue: cached-text matching,
  next/previous, wraparound, and cancellable bounded traversal (ADR-0125).
- [x] Local list sorting with presets/custom `tkfmt-1` expressions, reversing,
  and keep-first duplicate removal, each with undo/redo (ADR-0127).
- [x] Local M3U8 import/export, with relative-path resolution, offline/duplicate
  retention, and explicit rejection of unrepresentable references (ADR-0128).
  Export creates a new file; replacement and other playlist formats remain later work.
- [x] Expose the complete MPD stored-playlist browse/open/edit/save workflow
  in the current workspace (ADR-0129): sidebar list, server-keyed closable
  tabs, capability-gated server-round-trip edits, and idle-driven refresh.
  Multi-row reorder and restoring open playlist tabs remain follow-ups.
- [x] Committed search-result tabs in both authorities (ADR-0140): Enter in
  a library search keeps the current hits as a durable result tab — a new
  ordinary local list tab from the local library, a query-keyed session-only
  snapshot tab (ADR-0129 pattern) from MPD. The library-integrated live
  search remains the transient default. Expanding MPD album hits into
  complete releases inside the committed tab remains a follow-up.
- [x] Extend Find beyond cached display text (ADR-0142): every metadata
  value at every provenance, formatted durations, MPD `audio_format` and
  unknown protocol pairs — same bounded pipeline, with a uniform per-row
  value cap reported as a visible limit. Local codec/bit-rate/sample-rate
  search still needs probe-technicals retention on rows (follow-up).

Keep local list changes distinct from server-owned playlist mutations.
Removing duplicate list entries must not delete files.

References: [working lists and interchange](playback-library-conversion.md#working-lists-and-stored-playlists),
[MPD client](mpd-client.md).

## 2. Library filters and saved searches

**Proposal:** Extend artist/album browsing into useful collection views.

- [x] Structured filters (ADR-0150): the `tkq-1` dialect — foobar-inspired
  keyword surface, own normative spec — evaluates over a widened index
  (every tag value plus probed codec/sample-rate/bits/channels/duration)
  behind an explicit query toggle in the library panel; committed queries
  keep their results as ADR-0140 snapshot tabs. Old index rows backfill on
  their next Refresh.
- [ ] Saved searches, followed by query-backed autoplaylists.
- [ ] Idea: similarity-fed autoplaylists — use an online source such as
  Last.fm similar-artist/track data to queue related songs from the local
  index (network-gated, cached like the MusicBrainz client, never blocking
  library queries).
- [ ] Custom library grouping and tree expressions.
- [ ] Searchable CUE, chapter, and subsong titles in the index.
- [x] Parallel scan preparation (ADR-0151): probe and metadata reads run
  on a bounded worker pool while the walk and every guarded per-file
  commit stay serial; the library connection drops to WAL
  synchronous=NORMAL.
- [x] Standalone search dialog (ADR-0153): one non-modal surface over
  tkq-1 and word search with a database/current-tab scope switch;
  tab scope evaluates the shared row semantics, probes missing
  technicals on demand, and results flow to the standard destinations
  or a new tab. Rows retain probe technicals from dump time (ADR-0142
  follow-up closed for probed rows; the Find bar matches them).
- [ ] Idea: a headless scanner binary to run directly on the NAS that
  hosts the library — build the same sqlite index locally without the
  network bottleneck, then copy it into the workspace.

Example views: Jazz released after 1990, albums missing ReplayGain, and files
without MusicBrainz identifiers.

**Trackknife decision:** Filters and saved views operate on the cached index.
Filesystem scanning remains explicit: only pressing **Refresh** starts a scan,
as required by ADR-0116. Query reevaluation must not trigger filesystem scans.

Reference: [local library and current limits](local-library.md).
The existing [query-language sketch](query-language.md) requires reconciliation
with [compatibility.md](compatibility.md#searchquery-syntax) before a dialect is
chosen; external query-language compatibility is not an accepted requirement.

## 3. Complete album conversion

**Proposal:** A conversion should produce a complete album ready for use.

- [x] Carry artwork into converted output using qualified format mappings
  (ADR-0131): one resolved cover per source, embedded as FLAC `PICTURE`,
  ID3v2 `APIC`, or `METADATA_BLOCK_PICTURE`, verified byte-exactly before
  publication. Multi-picture carriage remains a follow-up.
- [x] Mirror the source folder structure beneath an explicit destination root
  (ADR-0132): byte-exact mirroring below the sources' inferred deepest common
  directory, with the planner's sanitization/collision/containment checks
  unchanged. An editable mirror root remains a follow-up.
- [x] Remove stale ReplayGain when processing changes the audio (ADR-0133):
  conversion strips `REPLAYGAIN_*`/`R128_*` fields from the transfer and
  verifies none survive in the output. Automatically rescanning converted
  outputs remains a follow-up tied to the M7 storage work.

The existing converter already supports codec presets, expression-based naming,
resampling, bit-depth choices, and text metadata transfer. Preserve its
verification, cancellation, and no-overwrite publication guarantees while
adding the missing pieces.

References: [converter specification](playback-library-conversion.md#converter),
[M8](../MILESTONES.md#m8--parallel-converter-resampler-and-organized-output).

## 4. Consistent tagging and artwork across formats

**Proposal:** Prioritize common collection formats before more obscure writers.

- [x] Qualify text tagging in MP4/M4A containers carrying AAC or ALAC
  (ADR-0136): standard atoms through TagLib's documented table, exact
  `com.apple.iTunes` freeform for everything else, box-level preservation
  proof (`ftyp`/`mdat` byte-identical) plus decoded-PCM equality, and
  `covr` survival. A dedicated ALAC fixture remains a follow-up.
- [x] Qualify MP3 and M4A artwork management (ADR-0137): ID3v2 APIC and
  MP4 covr join the FLAC adapter across inventory, donors/export/
  thumbnails/conversion carriage, the write plan, prepared-copy writers
  with the containers' preservation proofs, and the journaled commit.
  covr entries are untyped front covers; Ogg pictures remain open below.
- [ ] Extend artwork management to other supported containers as their
  preservation behavior is proven.

Qualified text writers cover FLAC, WavPack, MP3,
Vorbis, Opus, and MP4/M4A (ADR-0136), while qualified artwork editing covers FLAC, MP3, and MP4 (ADR-0137). Playback support
must remain distinct from write support. Each new writer needs real-file
round trips proving preservation of audio, unknown metadata, and container data.

Reference: [metadata and artwork](metadata-and-files.md).

## 5. Universal ReplayGain support

**Proposal:** Complete the path from measurement to durable storage and playback.

- [x] CUE sheet carriage (ADR-0139): scan results for CUE logical tracks
  persist as foobar2000-convention `REM REPLAYGAIN_*` lines in the sheet
  itself — routed through the write plan per sheet, published by a
  revision-gated atomic byte-preserving rewrite — and local playback
  consumes the sheet values ahead of the physical file's whole-file tags,
  including across gapless takeovers.
- [ ] Bring CUE sheet rewrites into the undo journal (content-kind schema
  migration plus recovery/undo branches; ADR-0139 follow-up).
- [x] Loudness sidecar (ADR-0141): scan results for non-CUE logical
  tracks (container chapters, codec subsongs, segments) persist into a
  versioned, human-inspectable `<file>.tkmeta` beside the source —
  routed through the write plan per file, merged by a revision-gated
  atomic commit, staleness-checked by size+mtime — and project onto
  probed rows at sidecar provenance, which local playback consumes
  ahead of CUE and embedded values.
- [x] Sidecar fallback for unwritable formats (ADR-0143): clean
  conventional ReplayGain on adapters without a safe tag writer
  (WAV/AIFF/APE/…) diverts into the whole-file sidecar entry during the
  planner's reader pass; other fields keep the visible writer block, and
  the outcome stays visible through provenance and the apply summary.
- [x] Journal parity for carrier rewrites (ADR-0145): CUE-sheet and
  existing-sidecar mutations run the full ADR-0059 lifecycle — journaled
  states, crash recovery (roll-forward with carrier-content verification,
  prepublication-debris rollback), retained undoable byte pre-images, and
  reconciliation reporting. Emptied sidecars publish as empty documents
  instead of vanishing; sidecar creation stays a direct atomic publish by
  explicit argument (no pre-image at risk).
- [x] Opus R128 (ADR-0149): the decoder reads RFC 7845 `R128_TRACK_GAIN`/
  `R128_ALBUM_GAIN` Q7.8 comments (relative to the output gain libopus
  already applies) and lifts them 5 dB onto the ReplayGain 2.0 scale,
  preferring them over `REPLAYGAIN_*` remnants; scans stage Q7.8 R128
  proposals for Opus tag writes (no peaks — the RFC defines none) while
  the sidecar-only policy keeps conventional fields; the provenance view
  shows the two R128 columns. Output-gain rewriting stays a future
  expert operation.
- [x] Add playback preamp controls (ADR-0138): separate ±20 dB preamps for
  tracks with and without loudness data, applied only while local
  ReplayGain is active, persisted, and inherited by every loaded source.
- [x] True-peak policy (ADR-0148): an opt-in "True peak as ReplayGain
  peak" checkbox proposes the oversampled true peak in the standard
  `REPLAYGAIN_*_PEAK` fields (sample peak stays the interoperable
  default); sidecar entries record the peak kind via an optional
  `peak_kind` member and the CSV export names it in a `peak_kind`
  column.
- [x] Result workflow (ADR-0146): a persisted "Store in sidecar only"
  policy routes loudness on every non-CUE source into the sidecar even
  for writable formats; failed or cancelled measurements re-run through
  a "Retry N failed" status link; and an "Album merging discs" grouping
  mode strips trailing disc designators so "Album (Disc 1)"/"Album CD2"
  measure as one programme.
- [x] Export and provenance (ADR-0147): scan results export as CSV
  through a status link (a snapshot of the measurement, independent of
  later draft edits), and a "Loudness sources…" view shows each track's
  effective ReplayGain values with their origin — draft, sidecar, CUE
  segment, or embedded — making the storage precedence tangible. The
  result-review surface is complete.
The [correctness prerequisite](#correctness-prerequisite) covers logical-track
scan propagation. Measurement, grouping, visible draft proposals, and ordinary
local playback gain modes already exist; this work completes storage and
coverage rather than implementing a new scanner.

References: [ReplayGain](replaygain.md),
[Properties scan construction](../src/bench/metadata_properties_dialog.cpp).

## 6. Linux desktop integration

**Proposal:** Make playback convenient while the window is in the background.

- [x] MPRIS integration (ADR-0135): `org.mpris.MediaPlayer2.trackknife`
  mirrors and steers exactly what the in-app transport is bound to — the
  active authority — with typed metadata, position/Seeked, and volume.
- [x] Media-key control while the application is unfocused: delivered
  through MPRIS, which modern Linux desktops use for media keys.
- [x] Optional desktop notifications (ADR-0144): a Playback-menu toggle
  (off by default, persisted) posts a quiet transient toast on track
  changes while the window is in the background, fed by the same
  authority-aware MPRIS now-playing snapshot. One replaced bubble, low
  urgency, silent degradation without a notification daemon.
  Notification artwork remains a follow-up.

Desktop controls must respect the established MPD/local playback authority
contract. Notification behavior should be optional and quiet by default.

Reference: [M9](../MILESTONES.md#m9--melody-endpoint-and-advanced-listening-mpd-authority).

## 7. Listening history and album-oriented playback

**Proposal:** Add listening memory and album-oriented discovery.

- [ ] Play counts, last-played timestamps, and ratings.
- [ ] Restore the last playback position without assuming automatic playback.
- [ ] Shuffle albums while preserving track order inside each album.
- [ ] Use listening statistics in views such as unplayed albums.
- [ ] Autoplay: when the list or queue ends, optionally continue with
  similar tracks instead of stopping — similarity from an online source
  (Last.fm similar artists/tracks first; source pluggable), mapped onto
  the local index or the connected server's library, never external
  streams. Strictly opt-in, network-gated, cached like the MusicBrainz
  client, and clearly marked in the queue so appended tracks are
  distinguishable from user choices. Related: the similarity-fed
  autoplaylist idea in [Area 2](#2-library-filters-and-saved-searches).

Statistics should follow stable track identity through file operations.
Writing them into audio tags requires explicit opt-in. Current Random playback
is track-oriented; album shuffle is a separate order mode.

Reference: [playback statistics](playback-library-conversion.md#playback-statistics).

## 8. Collection maintenance

**Proposal:** Help users identify collection problems before changing files.

- [ ] User-facing integrity scans with distinct decode and checksum findings.
- [ ] Duplicate-audio comparison beyond duplicate paths or matching tags.
- [ ] Missing-file relinking.
- [ ] Album-completeness checks with explicit evidence and uncertainty.

For example, two differently tagged files containing the same recording should
be discoverable without implying that they are interchangeable or safe to
delete. Any resulting filesystem mutation follows the existing preview,
conflict, commit, and recovery contracts.

Reference: [verification and diagnostics](playback-library-conversion.md#verification-and-diagnostics).

## Existing follow-ups outside the eight priorities

These remain open requirements or proposals from the broader specifications;
they are not additional immediate commitments.

- Workspace: expose the command palette and configurable shortcuts; add
  expression-defined track columns/grouping and saved metadata field layouts;
  add the planned jobs-and-errors (job center), console/diagnostics, queue
  inspector, and search/filter editor panels.
- Metadata: additional qualified writers, portable/custom filename
  sanitization, Unicode-normalization policy, and richer matching options;
  `TOTALTRACKS` totals for numbering (group counters landed in ADR-0104);
  general metadata sidecars beyond the loudness record; companion-file
  copy/move with previewed empty-folder cleanup.
- File-operation undo: cross-filesystem undo, changed-artifact undo, and the
  artwork undo chain on exchange-less filesystems (ADR-0111 addendum) remain
  recorded follow-ups without a current surface.
- Conversion: channel-processing policy, grouped/merge output, qualified
  DSP/gain, and splitting cue tracks into separate files. Downsample-only
  caps and keep-source depth landed in ADR-0134.
- Infrastructure: shared resource scheduling/retry, secure credential storage,
  user backup/restore, and representative large-library/network/device testing.
- Later product work: Melody playback endpoint, DSP graph, release hardening
  and packaging. Plugins, CD ripping, radio, remote import, simultaneous
  multiple MPD server sessions, and the cross-authority conveniences (opening
  a mapped server item locally, explicit MPD update after publication) remain
  deferred.

## Scope and maintenance

For album preparation, emphasize cover-preserving conversion and consistent
artwork support. For daily listening, emphasize queue undo/find/sort and
playlist support. Keep filesystem scans manual and MPD/local authorities
separate throughout.

The 2026-09-06 reconciliation replaces stale current-status summaries in the
feature matrix and documentation index. MusicBrainz, AcoustID, grouped
numbering, conventional local ReplayGain, and the baseline converter are
already implemented; their remaining gaps are listed above. The committed-
operation history/undo UI was intentionally removed in ADR-0084 and is not a
missing implementation promised by this roadmap. Draft undo is implemented;
local removal/reorder undo is implemented in ADR-0123.

Maintain current capability status in the feature matrix and implementation
priority here. Older dated milestone entries and ADRs remain historical
records; a retired shell's functionality is not evidence of a current UI.

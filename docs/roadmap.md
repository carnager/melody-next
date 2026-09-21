# Trackbench feature roadmap

Updated on 2026-09-16, through ADR-0178.

This page lists the work still to do. The [feature matrix](feature-matrix.md)
records what's implemented; [MILESTONES.md](../MILESTONES.md) keeps the milestone
status and history. M0-M10 are complete; this roadmap tracks post-release work.

**Proposal:** Keep the order below as a guide. Bugs in an existing workflow
come before adding more features to it. An item here is not a release promise.

## Correctness prerequisite

The reported CUE, chapter, and subsong ReplayGain selection bugs were fixed in
ADR-0124. CUE sheets and loudness sidecars now provide storage for those results;
existing carrier rewrites also have recovery and undo support (ADRs 0139,
0141, and 0145).

Library refresh now handles deleted folders, unavailable mounts, and cover
changes. Searches run against the index and never start a filesystem scan.
Schema 33 fixes incomplete field indexes that could produce false `MISSING`
results; older records need one explicit Refresh (ADR-0166).

## 1. Queue and playlist editing

Local lists already support find, sort, reverse, duplicate removal, and undo
for removal and rearrangement. M3U8 import/export and MPD playlist editing are
available. Searches can be kept in tabs, and local selections can be copied or
moved into new tabs from the menu or tab strip.

Still to do:

- [x] Undo for adding and replacing local-list contents (ADR-0176).
- [x] One coordinated undo/redo operation for the latest move between tabs;
  copies are undone in the destination list (ADR-0176).
- [ ] Reorder multiple MPD playlist tracks at once and restore open MPD playlist
  tabs after a restart.
- [ ] Expand album hits into complete releases inside committed MPD search tabs.
- [ ] Export over an existing playlist file with a reviewed replacement, and
  support playlist formats beyond M3U8.

Removing an entry from a list must never delete its file.

References: [working lists](playback-library-conversion.md#working-lists-and-stored-playlists),
[MPD client](mpd-client.md), [new-tab transfers](adr/0167-create-tabs-from-track-transfers.md).

## 2. Library filters and saved searches

ADR-0206 connects the existing dynamic-rule editor to local scan and committed
index changes, including changes during an outstanding query. Persistent live
tabs and custom library views remain open.

Structured `tkq-1` queries and named saved searches are implemented. The Search
dialog can search the library database or the current local tab. Opening database
results uses cached tags and technical information; covers load separately.
Both libraries show album counts before you expand an artist.

Still to do:

- [ ] Autoplaylists that update when the index changes. Saved searches currently
  run when opened; result tabs keep a snapshot.
- [ ] Custom grouping and expressions for the local library tree.
- [ ] Index CUE, chapter, and subsong titles as individually searchable tracks.
- [ ] An album-cover grid.

Two ideas need more design work: using similar-artist/track results to build a
list from your own collection, and a command-line scanner that can build the
index directly on a NAS. Neither should make an ordinary library query start
scanning files or contacting an online service.

References: [local library](local-library.md), [query language](query-language.md),
[saved searches](adr/0163-saved-search-definitions.md).

## 3. Extend album conversion

The converter carries one cover, transfers text tags, and can mirror source
folders or name output with `tkfmt-1`. It supports resampling, a downsample-only
limit, source/16/24-bit depth, channel conversion, and permanent Track/Album
ReplayGain. Stale ReplayGain tags are removed from converted audio.

Still to do:

- [ ] Carry multiple embedded images.
- [ ] Report which selected files have no usable cover.
- [ ] Scan converted output for fresh ReplayGain values.
- [ ] Design grouped chapter-container and merge-all output metadata/boundary
  semantics; CUE tracks already split into exact per-track files.
- [ ] Add a general ordered DSP graph if concrete non-mastering use cases
  justify it.

New options must keep the converter's output verification and collision checks.

References: [converter](playback-library-conversion.md#converter),
[M8](../MILESTONES.md#m8--parallel-converter-resampler-and-organized-output).

## 4. Consistent tagging and artwork across formats

Text editing supports FLAC, WavPack, MP3, Vorbis, Opus, and MP4/M4A.
Artwork editing supports FLAC, MP3, and MP4/M4A. Tag and artwork drafts can be
applied together, with per-file recovery and retry for unfinished files.

Still to do:

- [ ] Artwork editing in more containers, including Ogg.
- [ ] A dedicated ALAC fixture for MP4 text-writing tests.
- [ ] Deleting external cover files through a reviewed file operation.
- [ ] More text writers, each backed by real-file tests that check audio,
  unknown tags, and container data survive a rewrite.
- [x] Previewed Scripts actions for a field blocklist (remove named fields) and
  allowlist (remove every field except named fields), replacing the retired
  display-only field-layout UI from ADR-0177.

Playback support alone isn't enough evidence that a format can be edited safely.

Reference: [metadata and artwork](metadata-and-files.md).

## 5. Extend ReplayGain support

Track and album scanning, multi-disc grouping, optional true peak, failed-item
retry, CSV export, and a view of each value's source are implemented. Results
can be stored in supported tags, CUE sheets, or `.tkmeta` sidecars. Opus uses
R128 gain comments. Local playback has separate preamps for files with and
without loudness data.

ADR-0172 checks analysis across every decodable repository fixture and closes
the storage/playback coverage gate while keeping those claims separate: a file
may be scannable without having writable tags. Output-gain editing for Opus and
rescanning converted output remain optional future work.

References: [ReplayGain](replaygain.md),
[format coverage](feature-matrix.md#format-support-dimensions).

## 6. Linux desktop integration

MPRIS controls the player selected by the active tab, including when the window
is in the background. Desktop media keys use that interface. Optional track
notifications are available and are off by default.

- [ ] Include artwork in desktop notifications.

Reference: [desktop integration](feature-matrix.md).

## 7. Listening history and album-oriented playback

ADR-0207 collects qualified local listens through the serialized persistence
worker, deduplicates occurrences durably, and preserves physical/logical
identity across app-managed publications. ADR-0208 adds optional local Play count
and Last played columns. Next: implement opt-in paused resume, then album shuffle.
Resume storage exists but runtime
resume does not. Server statistics remain owned by the server.

- [x] Track and album ratings on the shared 0-10 star scale (ADR-0179):
  MPD rating stickers, Melody's native rating commands, and a local
  content-identity store that survives rescans and moves.
- [x] Collect local play counts and last-played timestamps (ADR-0207).
- [x] Display local play counts and last-played timestamps in track views (ADR-0208).
- [x] Display independently collected Melody server statistics behind `melody_stats` (ADR-0209).
- [x] Ratings as a search target: `rating`/`albumrating` tkq pseudo-fields in
  local queries and saved searches, and Melody rating filter terms in the
  server search box.
- [ ] Optional opt-in writing of ratings into file tags (POPM, `RATING`)
  through the previewed metadata write workflow.
- [ ] Restore playback position without automatically starting playback.
- [ ] Shuffle albums while keeping each album's tracks in order.
- [ ] Use listening statistics in queries, such as finding unplayed albums.
- [ ] Optional autoplay when a list ends, choosing related tracks from the local
  or server library. Online similarity lookups need explicit opt-in, caching,
  and a clear indication of which tracks were added automatically.

Statistics should follow a track when its file moves. Writing them into audio
tags requires opt-in. The current Random mode shuffles tracks, not albums.

Reference: [playback statistics](playback-library-conversion.md#playback-statistics).

## 8. Collection maintenance

- [ ] Integrity scans that distinguish decoding failures from checksum failures.
- [ ] Compare duplicate audio, beyond matching paths or tags.
- [ ] Relink missing files.
- [ ] Check album completeness and show the evidence for missing tracks.

Finding two copies of a recording should help someone review them. It should
not imply that one is safe to delete.

Reference: [verification](playback-library-conversion.md#verification-and-diagnostics).

## Existing follow-ups outside the eight priorities

These are recorded requirements or proposals, not extra commitments for the
next release.

- **Workspace:** add custom expression columns/grouping and the planned job,
  diagnostic, queue-inspector, and search-editor panels.
- **Metadata and paths:** possible new versioned sanitization/normalization
  policies (Linux and portable policies are available); richer typed matching;
  `TOTALTRACKS` when numbering; general metadata
  sidecars; and copying or moving companion files, including reviewed cleanup
  of empty folders.
- **File undo:** cross-filesystem undo, changed-artifact undo, and the artwork
  undo chain on filesystems without rename-exchange support.
- **Infrastructure:** shared job scheduling and retry, secure credential
  storage, backup/restore, and larger library/network/device test runs.
- **Later work:** a DSP graph, release packaging, plugins, CD ripping, radio,
  remote import, and simultaneous MPD connections. The Melody playback endpoint
  is complete in ADR-0174; its multi-machine stress campaigns belong to M10.
  Explicitly updating MPD after local file changes also remains open. Loading
  mapped server files into a local tab is available, and the MPD context
  menus' ReplayGain/Convert actions run that materialization (ADR-0180).
  Edit tags now opens the standard editor tab and sidebar file list directly
  over mapped files, without an intermediate local queue tab (ADR-0203).
  **Proposal:** let Melody read complete file metadata on demand on the server,
  with revision evidence for
  conflict-safe Apply. A later server-owned write path could remove the mount
  requirement while retaining preview and recovery guarantees. See
  [Task 8](../CODEX_TASKS.md#task-8--direct-melody-tag-editing-and-server-side-metadata--todo).

## Scope and maintenance

Keep current support and its limits in the feature matrix. Update this page
when a gap is closed, rather than leaving a completed feature on the to-do list.
Dated milestone entries and ADRs remain the history of how those decisions
were made.

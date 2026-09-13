# Trackknife knowledge base

This directory is the handoff package for humans and coding agents beginning
work on the Trackknife project, whose primary product is the unified
**Trackbench** MPD/Melody and local-file workspace (ADR-0058). The former
standalone **Trackknife** MPD client was retired in ADR-0071.

The ordered delivery plan is [`../MILESTONES.md`](../MILESTONES.md).
The [feature roadmap](roadmap.md) records the prioritized gaps identified in
the 2026-09-06 source review, with open implementation checklists.

## User guides

- [Melody setup](melody.md) — configure the server, connect Trackknife, and add speakers.
- [Local library](local-library.md#using-the-library) — add folders and search your collection.
- [Formatting and scripts](tkfmt.md) — naming patterns and tag transformations.

## Current continuation point

Last reconciled: 2026-09-06 against source baseline `bfc99e3`, ADRs through
0128, and persistence schema **28**.

Use the [feature matrix](feature-matrix.md) for current capability status and
[roadmap](roadmap.md) for prioritized open work. M5 remains the active
acceptance gate; MusicBrainz/AcoustID implementation has landed, and ReplayGain
and conversion are partially implemented. Milestone closure and individual
feature availability are separate claims.

The primary workspace is Trackbench, currently built as `trackknife` from
`src/bench`. The separate MPD shell was retired in ADR-0071. Its committed
search tabs and command-discovery UI are not automatically available in the
current workspace merely because older M3 records describe them;
stored-playlist tabs returned in ADR-0129.

Latest usability work (2026-09-13): ADR-0157 adds tag field review controls;
ADR-0158 adds MusicBrainz file/track assignment for untagged albums, broader
album search, and bounded throttle recovery. ADRs 0159–0160 add staged bulk artwork editing, unified tag/artwork Apply, and atomic per-file publication. See the feature matrix and
[metadata guide](metadata-and-files.md#reviewing-fields-and-identifying-untagged-albums).

Current implemented workflows include:

- Authority-bound MPD/local playback, queue operations, persistent local lists,
  shared track layouts, local playback modes, and conventional ReplayGain.
  Local removal/reorder undo is available through per-tab session history.
  Whole-list sort/reverse and duplicate removal also use named undo/redo.
  Ctrl+F finds cached text in local lists and the MPD queue; F3/Shift+F3 navigate
  matches. M3U8 import creates local lists with relative and unavailable file
  references; export writes whole-file lists to new M3U8 files (ADR-0128).
- MPD library browse/search with covers and queue actions, plus stored-playlist
  browse/open/edit/save in server-keyed tabs with capability-gated server
  round trips (ADR-0129). Local Library offers
  indexed browse/search, track numbers, covers, and drag/drop. Filesystem scans
  run **only on Refresh**; in-app metadata/path commits update cached records.
- Properties with multi-value drafts, undo/redo, saved transformations,
  grouped numbering, MusicBrainz/AcoustID identification, and Cover Art Archive.
  Automatic scripts stage visible edits; Apply writes the existing draft.
- Qualified FLAC/WavPack/MP3/Vorbis/Opus text writers, FLAC artwork management,
  journaled metadata/path publication, and automatic recovery. Embedding a cover
  preserves pending tag drafts; publication preserves playback progression.
- Bounded loudness measurement and draft staging, plus conversion to qualified
  FLAC/Opus/MP3/Vorbis targets with naming presets, resampling, bit-depth options,
  text transfer, output verification, and limited-filesystem fallbacks.

Immediate gaps include broader list history and playlist interchange, committed
MPD search tabs, advanced library views, conversion artwork/stale-loudness
handling, additional format writers, and complete ReplayGain storage/coverage. The
Properties-to-scanner logical-source path is regression-tested (ADR-0124);
logical-track measurements still require a future durable storage target.
See the roadmap for priorities and the matrix for exact limits.

Feature-specific starting points:

- [Local library](local-library.md), [MPD client](mpd-client.md), and
  [workspace](ui-workspace.md).
- [Metadata and files](metadata-and-files.md), [ReplayGain](replaygain.md), and
  [playback/conversion](playback-library-conversion.md).
- [Architecture](architecture.md) and [accepted decisions](adr/).

Older milestone progress entries and the dated sections of
[open decisions](open-decisions.md) are historical context. Resolve conflicts
using newer accepted ADRs and the current implementation. Historical test counts
are evidence for their recorded revisions, not assertions about the latest suite.

## Reading order

1. [`product.md`](product.md) — identity, principles, scope, and priorities.
2. [`compatibility.md`](compatibility.md) — what “in the spirit of foobar2000”
   does and does not mean.
3. [`mpd-client.md`](mpd-client.md) — MPD sessions, source mapping, live queue,
   queue/list tabs, and Melody capabilities.
4. [`feature-matrix.md`](feature-matrix.md) — current implementation and limits;
   [`roadmap.md`](roadmap.md) — prioritized open work.
5. [`ui-workspace.md`](ui-workspace.md) — default MPD layout, queue/list tabs,
   reusable views, and performance budgets.
6. [`m3-validation.md`](m3-validation.md) — automated evidence and final live
   MPD/Melody acceptance pass for the current milestone.
7. [`title-formatting.md`](title-formatting.md) — the versioned Trackknife
   formatting language and its implementation model.
8. [`metadata-and-files.md`](metadata-and-files.md) — tagger, bulk transforms,
   artwork, renaming, moving, and transaction safety.
9. [`replaygain.md`](replaygain.md) — analysis, storage, playback, and conversion.
10. [`playback-library-conversion.md`](playback-library-conversion.md) — remote/
   local playback, server library, lists, conversion, and integrity tools.
   [`local-library.md`](local-library.md) specifies the optional indexed local
   collection and its refresh/availability rules.
11. [`query-language.md`](query-language.md) — deferred deeper search and
   autoplaylist design.
12. [`architecture.md`](architecture.md) — subsystem boundaries and
   unresolved technology choices.
13. [`open-decisions.md`](open-decisions.md) — choices that still need ADRs.
14. [`adr/`](adr/) — accepted and proposed architecture decisions.
15. [`sources.md`](sources.md) — source register and research caveats.

## Status vocabulary

- **Required**: product requirement already established in conversation.
- **Compatibility requirement**: observable external behavior to reproduce.
- **Proposed**: recommended Trackknife design awaiting implementation feedback.
- **Deferred**: valuable, but not necessary for the first useful release.
- **Unknown**: must be decided or measured.

External products may inform the design, but Trackknife behavior is defined by
these specifications, ADRs, and executable tests unless a document explicitly
declares a compatibility requirement.

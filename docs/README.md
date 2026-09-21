# Documentation

## Using Trackknife

- [Melody setup](melody.md): connect to the server and set up speakers.
- [Local library](local-library.md#using-the-library): add folders, browse albums,
  search, and save searches.
- [Last.fm accounts and scrobbling](lastfm.md): independent local/server accounts and Love/Unlove.
- [Dynamic playlists](dynamic-playlists.md): shared local/server rules and Last.fm sources.
- [Search syntax](query-language.md): field filters such as
  `bitspersample EQUAL 24` and `REPLAYGAIN_ALBUM_GAIN MISSING`.
- [Formatting and scripts](tkfmt.md): naming patterns and tag transformations.
- [Tagging and artwork](metadata-and-files.md#reviewing-fields-and-identifying-untagged-albums):
  review edits, match MusicBrainz tracks, and manage covers.

The [README](../README.md) has build instructions and screenshots. The more
technical documents below describe how the features work and where their
limits are.

## Current state

Updated on 2026-09-22, through ADR-0215 and database schema 40.

- [ADR-0215: Listening-history queries](adr/0215-history-library-queries.md).
- [ADR-0214: Continuous album playback](adr/0214-continuous-album-playback.md).
- [ADR-0213: Album shuffle for every Melody list](adr/0213-melody-list-album-shuffle.md).
- [ADR-0212: One-shot album shuffle](adr/0212-one-shot-album-shuffle.md).

- [ADR-0211: Interrupted request resume](adr/0211-interrupted-request-resume.md).

- [ADR-0210: Paused playback resume](adr/0210-paused-playback-resume.md).

- [ADR-0209: Melody listening statistics](adr/0209-melody-listening-statistics.md).

The application is built as `trackknife` from `src/bench`. Older development
documents call this workspace **Trackbench**. It combines the MPD client and
local file tools in one window; the separate MPD application was retired in
ADR-0071.

Recent work includes saved searches, opening search results from cached
metadata, MusicBrainz matching for untagged albums, and saving tag and cover
changes together. Local tracks can be copied or moved into a new tab from the
context menu or by dropping them on the tab strip. Both libraries show artist
album counts loaded in the background.

M5 local tagging/file operations, M6 MusicBrainz identification, M7 ReplayGain,
M8 conversion, the M9 Melody endpoint, and M10 hardening are complete.
Post-release workspace improvements are active. The [feature matrix](feature-matrix.md)
lists what works and its restrictions. The [roadmap](roadmap.md) lists the
remaining work.

## Working on the code

Start with [AGENTS.md](../AGENTS.md) for repository rules, then read:

1. [Milestones](../MILESTONES.md), [product scope](product.md), and
   [compatibility](compatibility.md).
2. The relevant feature document from the list below.
3. [Architecture](architecture.md) and the related [design decisions](adr/).

A development build and test run:

```sh
cmake --preset dev
cmake --build --preset dev
QT_QPA_PLATFORM=offscreen ctest --preset dev
cmake --build build/dev --target format-check
bash scripts/check_spdx.sh
```

Run these from the repository root. Sanitizer and static-analysis presets are
also available in [CMakePresets.json](../CMakePresets.json).

## Feature references

- [Up Next](up-next.md): temporary requests with automatic return to normal
  playback in local and Melody contexts; [design](adr/0196-up-next-request-queue.md).
- [MPD client](mpd-client.md): connections, the server library, queues, and playlists;
  [shared server-list interactions](adr/0198-shared-server-list-interactions.md).
- [Workspace](ui-workspace.md): tabs, views, controls, and performance requirements.
- [Local library](local-library.md): indexing, offline folders, refresh, and searches.
- [Metadata and files](metadata-and-files.md): tag drafts, artwork, rename/move, and recovery.
- [ReplayGain](replaygain.md): measurement, storage, and playback gain.
- [Playback and conversion](playback-library-conversion.md): local playback,
  working lists, conversion, and planned collection tools.
- [Query language](query-language.md): the `tkq-1` grammar and evaluation rules.
- [Title formatting](title-formatting.md): the `tkfmt-1` language specification.
- [Open decisions](open-decisions.md): questions that still need a decision.
- [Release checklist](release-checklist.md): packaging, legal, accessibility,
  stress, backup/restore, and end-to-end acceptance gates.
- [M10 validation](m10-validation.md): hardening evidence and the per-artifact
  release boundary.
- [Post-release roadmap](roadmap.md): remaining prioritized work, updated
  through ADR-0178.
- [Sources](sources.md): references used when designing and checking behavior.

The dated milestone notes, [M3 validation](m3-validation.md), and older ADRs
record work at that point in time. Use the feature matrix for current status;
an old test result or screenshot doesn't establish what the current app supports.

In specifications, **Trackknife decision** means behavior chosen for this app,
**Compatibility requirement** means behavior it must match elsewhere,
**Proposal** means a suggested direction, and **Unknown** means it still needs
research or a decision.

- [ADR-0202: Automatic Last.fm authorization](adr/0202-lastfm-automatic-authorization.md).

- [ADR-0203: Direct mapped tag editor](adr/0203-direct-mapped-tag-editor.md).

# Documentation

## Using Trackknife

- [Installation](install.md) and [setup](setup.md).
- [Engines, agents and the phone](melody.md): how they connect, a headless
  `melodyd`, and `melody-cli`.
- [Local library](local-library.md#using-the-library): add folders, browse albums,
  search, and save searches.
- [Last.fm accounts and scrobbling](lastfm.md): independent local/server accounts and Love/Unlove.
- [Dynamic playlists](dynamic-playlists.md): rules and Last.fm sources, on either library.
- [Search syntax](query-language.md): field filters such as
  `bitspersample EQUAL 24` and `REPLAYGAIN_ALBUM_GAIN MISSING`.
- [Formatting syntax](formatting.md): fields, functions and examples; the full
  reference is [tkfmt.md](tkfmt.md).
- [Tagging and artwork](metadata-and-files.md#reviewing-fields-and-identifying-untagged-albums):
  review edits, match MusicBrainz tracks, and manage covers.

The more technical documents below describe how the features work and where
their limits are.

## Current state

Updated on 2026-09-26, through ADR-0232 and database schema 45.

**The unified engine is in place.** `melodyd` owns the library and playback
and speaks protocol v1 over a socket or TCP. Trackknife, the Android app and
`melody-cli` are its clients, and audio comes out of output agents: the
engine's own speakers, other machines and phones. This supersedes the
two-authority model in older ADRs and in `architecture.md`.
[The migration plan](unified-engine.md) says how far along each phase is;
[melody.md](melody.md) says how to run it.

- [ADR-0232: melody-watch, telling the engine what changed on a NAS](adr/0232-melody-watch.md).
- [ADR-0231: The Android client](adr/0231-android-client.md).
- [ADR-0230: Opus streams and download tickets](adr/0230-opus-streams-and-download-tickets.md).
- [ADR-0229: Engines find each other and play for each other](adr/0229-engines-find-each-other.md).
- [ADR-0228: Output agents on protocol v1](adr/0228-output-agents-on-protocol-v1.md).
- [ADR-0227: This computer's engine and a remote one](adr/0227-local-and-remote-engines.md).
- [ADR-0226: Trackknife runs its own engine](adr/0226-trackknife-runs-its-own-engine.md).
- [ADR-0225: Cover size limits](adr/0225-cover-size-limits.md).
- [ADR-0224: Retire the MPD backend](adr/0224-retire-the-mpd-backend.md).
- [ADR-0223: TCP transport and authentication](adr/0223-tcp-transport-and-authentication.md).
- [ADR-0222: Protocol v1 framing and envelope](adr/0222-protocol-v1-framing.md).

- [ADR-0221: Entry identity and track identity](adr/0221-entry-and-track-identity.md).

- [ADR-0220: Unified engine and remote agents](adr/0220-unified-engine-and-remote-agents.md).

- [ADR-0219: Direct mapped Convert and ReplayGain](adr/0219-direct-mapped-conversion-and-replaygain.md).

- [ADR-0218: Explicit permanent conversion gain](adr/0218-explicit-permanent-conversion-gain.md).

- [ADR-0217: Grouped search presets](adr/0217-grouped-search-presets.md).

- [ADR-0216: History ordering and dynamic result interactions](adr/0216-history-sorting-and-dynamic-result-interactions.md).

- [ADR-0215: Listening-history queries](adr/0215-history-library-queries.md).
- [ADR-0214: Continuous album playback](adr/0214-continuous-album-playback.md).
- [ADR-0213: Album shuffle for every Melody list](adr/0213-melody-list-album-shuffle.md).
- [ADR-0212: One-shot album shuffle](adr/0212-one-shot-album-shuffle.md).

- [ADR-0211: Interrupted request resume](adr/0211-interrupted-request-resume.md).

- [ADR-0210: Paused playback resume](adr/0210-paused-playback-resume.md).

- [ADR-0209: Melody listening statistics](adr/0209-melody-listening-statistics.md).

The application is built as `trackknife` from `src/bench`. Older development
documents call this workspace **Trackbench**. Its library and playback belong
to an engine ([unified engine](unified-engine.md)); the MPD client it used to
contain was retired in [ADR-0224](adr/0224-retire-the-mpd-backend.md).

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
- [MPD client](mpd-client.md) *(retired, ADR-0224)*: the former MPD backend,
  kept for history.
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

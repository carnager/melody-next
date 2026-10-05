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

Updated on 2026-10-05, through ADR-0269: database schema 53, protocol 1 at
level 6 ([protocol levels](protocol-levels.md)).

**The unified engine is in place.** `melodyd` owns the library and playback
and speaks protocol v1 over a socket or TCP. Trackknife, the Android app and
`melody-cli` are its clients, and audio comes out through outputs: the
engine's own speakers, agents on other machines and phones, and optional UPnP
renderers. File work -- tagging, ReplayGain, conversion, moves -- is the
engine's too, on its own machine. This supersedes the two-authority model in
older ADRs and in `architecture.md`.
[The migration plan](unified-engine.md) says how far along each phase is;
[melody.md](melody.md) says how to run it.

- [ADR-0271: The protocol as a WebSocket, for clients behind a web proxy](adr/0271-the-protocol-as-a-websocket.md).
- [ADR-0270: The phone reaches an engine through a TLS proxy](adr/0270-the-phone-reaches-an-engine-through-a-tls-proxy.md).
- [ADR-0269: Up Next is the engine's, and the window shows it](adr/0269-up-next-is-the-engines.md).
- [ADR-0268: What shows a library follows its changes](adr/0268-the-library-follows-its-files.md).
- [ADR-0267: One Trackknife per workspace](adr/0267-one-trackknife-per-workspace.md).
- [ADR-0266: Undo copies where the person says, restored without an exchange](adr/0266-undo-copies-where-the-person-says.md).
- [ADR-0265: The matcher offers every selected file](adr/0265-matcher-offers-every-selected-file.md).
- [ADR-0264: Settings by subject, every engine's folders in one place](adr/0264-settings-by-subject.md).
- [ADR-0263: What was written can be undone](adr/0263-undo-what-was-written.md).
- [ADR-0262: One Apply for both windows, ReplayGain an Apply action](adr/0262-one-apply-for-both-windows.md).
- [ADR-0261: Identifying many albums at once](adr/0261-identify-many-albums.md).
- [ADR-0260: Protocol versions, and what 1.x keeps compatible](adr/0260-protocol-version-and-compatibility.md).
- [ADR-0259: The window keeps no lists of its own](adr/0259-the-window-keeps-no-lists.md).
- [ADR-0258: Dynamic rules pick groups, the engine selects, and some ship](adr/0258-dynamic-rules-pick-groups-and-ship-defaults.md).
- [ADR-0257: The tagger opens from the library, and reads the files behind it](adr/0257-tagger-opens-from-the-library.md).
- [ADR-0256: Lists travel as edits, and play by reference](adr/0256-lists-travel-as-edits.md).
- [ADR-0255: CoreAudio output on macOS](adr/0255-coreaudio-output-on-macos.md).
- [ADR-0254: Library views are levels of tkfmt-1 expressions](adr/0254-library-views.md).
- [ADR-0253: A list can continue with a dynamic playlist's rule](adr/0253-list-continuation.md).
- [ADR-0252: The Qt Quick window is retired](adr/0252-retire-the-qt-quick-window.md).
- [ADR-0251: Trackknife scales with the screen, and an interface size on top](adr/0251-interface-size-and-hidpi.md).
- [ADR-0250: The widgets window is drawn as the Qt Quick window is](adr/0250-widgets-window-draws-as-quick.md).
- [ADR-0249: A file renamed into place may come back with a new inode number](adr/0249-renames-that-renumber-files.md).
- [ADR-0248: A tag save's backup is a copy where the filesystem refuses hard links](adr/0248-copied-backup-where-hard-links-are-refused.md).
- [ADR-0247: Trackknife ships a light and a dark colour scheme](adr/0247-light-and-dark-color-schemes.md).
- [ADR-0246: ReplayGain is one setting, for every engine the window reaches](adr/0246-replaygain-is-one-setting-for-every-engine.md).
- [ADR-0245: Ratings can also be copied into a backup tag](adr/0245-rating-backup-tag.md).
- [ADR-0244: Tagging scripts convert a player's rating into FMPS_RATING](adr/0244-rating-conversion-step.md).
- [ADR-0243: tkfmt-1 gains `$decimal`](adr/0243-tkfmt-1-decimal.md).
- [ADR-0242: The language references ship inside Trackknife](adr/0242-shipped-language-references.md).
- [ADR-0241: The Raw script tab is the tagging script in Trackknife's own terms](adr/0241-native-raw-tagging-scripts.md).
- [ADR-0240: The Qt Quick window is a second view of one workspace, in Fusion everywhere](adr/0240-qt-quick-window.md).
- [ADR-0239: A desktop agent asks for Opus when its engine is far away](adr/0239-streams-follow-the-route.md).
- [ADR-0238: The Actions button opens a popover, and remembers](adr/0238-actions-popover.md).
- [ADR-0237: File work moves into the engine](adr/0237-file-work-in-the-engine.md).
- [ADR-0236: Playback states are numbered, and say what the output was told](adr/0236-playback-states-are-numbered.md).
- [ADR-0235: UPnP renderers as outputs](adr/0235-upnp-renderers-as-outputs.md) — implemented and optional.
- [ADR-0234: Engines are connections, not a switch](adr/0234-engines-are-connections.md).
- [ADR-0233: Lists live in the engine](adr/0233-lists-live-in-the-engine.md).
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
to its engines ([unified engine](unified-engine.md)); the MPD client it used
to contain was retired in [ADR-0224](adr/0224-retire-the-mpd-backend.md).

Milestones M5–M10 (tagging, MusicBrainz, ReplayGain, conversion, hardening)
are complete, and so is M11, the unified engine, as ADR-0259 has it. What 1.0
still needs is in the [roadmap](roadmap.md#toward-10). The [feature matrix](feature-matrix.md)
lists what works and its restrictions. The [roadmap](roadmap.md) lists the
remaining work.

Documents written for the older two-authority model -- an MPD side and a local
side in one window -- say so at the top, with what still holds.

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
  playback, held by the engine that plays; [design](adr/0196-up-next-request-queue.md).
- [MPD client](mpd-client.md) *(retired, ADR-0224)*: the former MPD backend,
  kept for history.
- [Workspace](ui-workspace.md): tabs, views, controls, and performance requirements.
- [Local library](local-library.md): indexing, offline folders, refresh, and searches.
- [Metadata and files](metadata-and-files.md): tag drafts, artwork, rename/move, and recovery.
- [ReplayGain](replaygain.md): measurement, storage, and playback gain.
- [Playback and conversion](playback-library-conversion.md): working lists,
  conversion, and planned collection tools; its playback parts are partly
  historical.
- [Query language](query-language.md): the `tkq-1` grammar and evaluation rules.
- [Title formatting](title-formatting.md): the `tkfmt-1` language specification.
- [Tagging scripts](tagging-scripts.md): a tagging script as text, one
  `$`-statement per step.
- [Open decisions](open-decisions.md): questions that still need a decision.
- [Release checklist](release-checklist.md): packaging, legal, accessibility,
  stress, backup/restore, and end-to-end acceptance gates.
- [M10 validation](m10-validation.md): hardening evidence and the per-artifact
  release boundary.
- [Roadmap](roadmap.md): remaining prioritized work and what 1.0 still needs,
  updated through ADR-0269.
- [Sources](sources.md): references used when designing and checking behavior.

The dated milestone notes, [M3 validation](m3-validation.md), and older ADRs
record work at that point in time. Use the feature matrix for current status;
an old test result or screenshot doesn't establish what the current app supports.

In specifications, **Trackknife decision** means behavior chosen for this app,
**Compatibility requirement** means behavior it must match elsewhere,
**Proposal** means a suggested direction, and **Unknown** means it still needs
research or a decision.

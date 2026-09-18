# Product definition

The Trackknife project builds **Trackbench**, the Qt 6 application in
`src/bench`, installed as `trackknife`. It combines an MPD/Melody client with
local playback and file tools in one Linux window.

The active tab decides which player the controls operate. In an MPD tab, the
server owns the queue, library, playlists, and outputs. In a local tab,
Trackbench plays files through PipeWire and offers tagging, MusicBrainz lookup,
ReplayGain, conversion, and file operations. Local editing commands are never
available for server queue entries.

The main references are foobar2000 for collection tools and Cantata for MPD
browsing. The useful ideas are tabbed lists, fast keyboard access, gapless
playback, and control over your own files. The app has its own interface and
formatting language; it does not load foobar2000 components.

A collection can be used directly from folders or added to the optional local
index. Scanning is explicit. Searching the index, opening results, and browsing
albums should not require rereading the collection.

The sections below describe product priorities and requirements. For what is
currently implemented, including restrictions, see the [feature matrix](feature-matrix.md).

## Priorities

### Priority 1: MPD playback and library browsing

- Reliable profiles, authentication, reconnect, capability discovery, and
  visible connection state.
- Fast server-library browsing and search with MusicBrainz-aware sorting and
  grouping where metadata permits it.
- Responsive transport, now playing, seek, volume, playback modes, ReplayGain
  mode, and output selection.
- Live-queue editing using stable song IDs and incremental updates.
- Multiple queue/list tabs for scratch work, named lists, stored playlists, and
  the live server queue.
- A default layout that works with mouse or keyboard without configuration.

Support standard MPD first. Show Melody's additional outputs and controls when
the server advertises them. Local playback keeps its own player and outputs.

### Priority 2: local playback workspace

- Local playback through FFmpeg and PipeWire: gapless transitions, exact seeking,
  per-list progression, volume, and device selection.
- Album-grouped track lists that can open files directly, without a library scan.
- Consistent tabs, shortcuts, and command access across local and MPD views.

### Priority 3: metadata, MusicBrainz, and ReplayGain

- A non-modal, spreadsheet-like tag workspace built for many tracks and fields.
- Type-to-add fields, fast fuzzy field lookup, direct keyboard navigation,
  focused display filters, and previewed bulk transformations.
- Arbitrary ordered multi-value metadata and complete MusicBrainz identifier/
  sort metadata preservation.
- Online MusicBrainz identification and metadata proposals with provenance and
  confidence, entering the staged preview as explicit network operations.
- Correct track and album ReplayGain analysis, sample and optional true peak,
  review before writing, and sidecar fallback when a safe embedded mapping is
  absent.
- Previewed, conflict-detecting, recoverable file and metadata mutation.

### Priority 4: converter and organized output

- FFmpeg-backed bounded parallel conversion with useful codec/device presets
  and high-quality resampling.
- Preserve the source directory structure or generate a relative destination
  using a preset or `tkfmt-1` expression.
- Channel/bit-depth policy, dither, metadata/artwork mapping, optional DSP,
  verification, and output ReplayGain.
- Complete path/conflict preview and atomic publication, including into an
  MPD music root by plain filesystem access.

### Priority 5: Melody output endpoint

Trackbench can later register as a Melody streaming endpoint
and play the server-provided stream through the shared local audio engine. This
comes after the existing local and server playback paths are finished.

## Established requirements

- One native Linux Qt 6 Widgets application, with no Wine dependency. The
  separate MPD application was retired in ADR-0071.
- Standard MPD compatibility before optional Melody extensions.
- MPD is authoritative for its database, current queue, stored playlists,
  transport, and outputs. The MPD client is one of potentially several
  connected clients and must follow changes made by the others.
- Only the server controls issue MPD commands. The local library caches file
  metadata; a local index entry does not mean the file belongs to MPD's library.
- Tabs keep queues and working lists accessible, with persistence appropriate
  to the kind of list.
- Local paths remain raw OS paths internally and need not be valid UTF-8.
- MusicBrainz identifiers, artist credits, sort names, release/disc identity,
  and related metadata remain intact and influence useful default organization.
- One versioned, pure `tkfmt-1` language serves display, sorting, grouping, and
  conversion/file naming in both authorities.
- FFmpeg is the common decode/encode backbone; PipeWire is the primary local
  output backend.
- Long work is asynchronous, cancellable, progress-reporting, and bounded.
- Metadata, ReplayGain, conversion, and filesystem writes use complete previews,
  revision checks, conflict detection, verification, and recovery journals.
- File operations accept only contained, revalidated local sources.

## Product principles

### Keep server and local work separate

The two players share views, shortcuts, and formatting rules. The active tab
must make it clear which player a command will affect. Moving a local list
entry must not change an MPD queue or move a file on disk.

### Make lists useful for ongoing work

Tabs let someone keep a playlist, review search results, or set aside an album
for tagging. Local lists persist. The live MPD queue follows the server, and
stored playlists remain server-owned.

### Include the collection tools

Tagging, MusicBrainz lookup, ReplayGain, conversion, artwork, and file operations
belong in the application. They should not require a plugin installation.

### Show changes before writing

Metadata and file operations must show their proposed changes, affected files,
and conflicts before committing. Execution must use the same plan that was
reviewed.

### Keep the interface responsive

Network requests, filesystem work, decoding, tag parsing, artwork loading, and
bulk formatting run off the UI thread. Long jobs need progress and cancellation.
The [workspace specification](ui-workspace.md) sets the performance budgets.

### Preserve the user's files

Rewrites must preserve unknown tags and container data. Requested timestamps,
list references, and file relationships must survive file operations. Failures
need a useful explanation and a recovery path.

### Put detail where it helps

Common tasks should take few steps. Per-file values, transformation rules, and
conversion settings should be available when someone needs to inspect or
change them.

### Use desktop-sized controls

Common actions should take one click or keystroke. Keep enough tracks and
fields visible to work on an album without constant scrolling. Menus and
popups are for less frequent choices; the layout should not need the spacing
of a touch interface.

### Specify the scripting behavior

Every `tkfmt-1` construct needs executable test cases. Changing the meaning of
a saved expression requires a new dialect version. Similar syntax does not
make it compatible with another player's scripts.

## MusicBrainz-aware organization

- Preserve recording, track, release, release-group, artist, work, and disc IDs.
- Keep credited names separate from sort names and canonical identities.
- Use release IDs for grouping when available, with predictable fallbacks for
  files that don't have them.
- Sort multi-disc releases by disc and track position.
- Let artist, album, and release selections be added to lists.
- Do not replace a user's credited display name just to normalize an identity.

MusicBrainz results enter the tag draft for review, with their source and
matching confidence. The provider cannot write files directly. Online lookup
and fingerprinting require an explicit request.

## Interface requirements

These are requirements for the finished interface; the command palette and
job center are still on the roadmap.

- The default window makes both server and local work available without
  building a layout first.
- Important actions have a menu or command-palette entry and a keyboard path.
- Track lists support selection, rearrangement, add-next/end, removal, crop,
  transfer between tabs, sort, reverse, randomize, and total duration.
- Tag editing supports typing field names, keyboard navigation, and applying
  changes to a selection without clicking every cell.
- Long tasks belong in a job center, without a modal progress dialog blocking
  the rest of the application.
- Transient errors should leave the rest of the work usable, keep successful
  results, and allow retrying the failed items.

## Anti-goals

The project is not an MPD server, a streaming-service shop, a DAW, a waveform
editor, or a mastering suite. It does not reproduce foobar2000's interface,
component ABI, or scripting behavior.

It must not require an index before files can be used, mix server entries and
local files in a queue, or infer permission to edit a local file from a server
path. Metadata cannot be reduced to one string per field. Decoding a format
does not establish support for writing it, and collection work must not block
the UI thread.

## Primary users and jobs

An MPD listener needs to connect, browse, manage playlists and the queue, and
choose outputs. The app must keep up when another client changes the server.

Someone maintaining a collection needs to listen to new files, identify an
album, edit tags and covers, scan ReplayGain, and rename or convert the files.
Identifiers and unrelated metadata must survive that work.

Someone converting music needs reusable codec and naming presets, resampling,
clear metadata handling, output verification, and eventually DSP and fresh
output loudness values.

## First public releases

The first releases need reliable MPD connections, browse/search, playlists,
queue editing, transport, and output controls. Local playback must be gapless,
and tagging, MusicBrainz matching, ReplayGain, and conversion must be dependable
with their documented formats.

The local library already exists, but it was not a release prerequisite.
Plugins, a Melody playback endpoint, and an upload protocol are also not
prerequisites for releasing the player and file tools.

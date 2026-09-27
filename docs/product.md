# Product definition

**Trackknife** is a Qt 6 application for Linux, built from `src/bench` and
installed as `trackknife`. **Melody** is the family of programs around it: the
engine `melodyd`, output agents, `melody-cli`, `melody-watch` and the Android
app.

An engine owns a library, its lists and playback, and keeps playing when every
client is closed (ADR-0220). Trackknife is a client of engines -- this
computer's, and any others you list, each with its own library and lists
(ADR-0234). Sound comes out of output agents: an engine's own speakers,
another computer, a phone. Trackknife also does the collection work --
tagging, MusicBrainz lookup, ReplayGain, conversion and file operations -- on
files it can reach, including an engine's files over a mount.

The main reference is foobar2000 for collection tools. The useful ideas are
tabbed lists, fast keyboard access, gapless playback, and control over your
own files. The app has its own interface and formatting language; it does not
load foobar2000 components.

Files can be used directly from folders or added to an engine's library.
Scanning is explicit. Searching the library, opening results, and browsing
albums do not reread the collection.

The sections below describe product priorities and requirements. For what is
currently implemented, including restrictions, see the [feature matrix](feature-matrix.md).

## Priorities

### Priority 1: playback and library browsing on every engine

- Reliable connections to several engines at once, with passwords,
  reconnection and visible state; engines found on the network.
- Fast library browsing and search with MusicBrainz-aware sorting and
  grouping where metadata permits it.
- Responsive transport, now playing, seek, volume, playback modes, ReplayGain
  mode, and choosing where the music plays.
- Gapless playback through FFmpeg, exact seeking, and following changes other
  clients make -- another window, the phone, a script.
- Tabs for scratch work, named lists and searches, each belonging to one
  engine, with the same operations on every engine's lists.
- A default layout that works with mouse or keyboard without configuration.

### Priority 2: playing anywhere

- Output agents: an engine plays on its own speakers, on another computer, or
  on a phone, and moves the music between them where it is.
- The phone as remote control and as speaker, with Opus over mobile data and
  albums kept for offline listening.
- One engine plays at a time: starting one stops the other.

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
  engine's music folder over a mount.

## Established requirements

- One native Linux Qt 6 Widgets application, with no Wine dependency.
- The engine is authoritative for its library, lists, queue, playback and
  outputs. Every client is one of several and must follow changes made by the
  others. Features are built once, not once per location (ADR-0220).
- The UI asks and the engine answers: Trackknife holds no library database of
  its own and does not decide what an engine plays.
- Tabs keep lists accessible; a tab belongs to one engine, and a list mixing
  engines is not planned.
- Local paths remain raw OS paths internally and need not be valid UTF-8.
- MusicBrainz identifiers, artist credits, sort names, release/disc identity,
  and related metadata remain intact and influence useful default organization.
- One versioned, pure `tkfmt-1` language serves display, sorting, grouping,
  conversion and file naming, in Trackknife, the engine and `melody-cli`.
- FFmpeg is the common decode/encode backbone; PipeWire is the primary audio
  output on Linux.
- Long work is asynchronous, cancellable, progress-reporting, and bounded.
- Metadata, ReplayGain, conversion, and filesystem writes use complete previews,
  revision checks, conflict detection, verification, and recovery journals.
- File operations accept only contained, revalidated local sources.

## Product principles

### One implementation, whichever engine

A list on this computer's engine and one on a NAS have the same operations.
What differs is only which engine a tab's list came from, and a tab says so.
Moving a list entry never moves a file on disk.

### Make lists useful for ongoing work

Tabs let someone keep a playlist, review search results, or set aside an album
for tagging. Lists live in their engine, so every client sees the same ones.

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

- The default window makes every engine's library and lists available
  without building a layout first.
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

The project is not a streaming-service shop, a DAW, a waveform editor, or a
mastering suite. It does not reproduce foobar2000's interface, component ABI,
or scripting behavior.

It must not require an index before files can be used, mix engines' entries
in one queue, or infer permission to edit a file from another machine's path. Metadata cannot be reduced to one string per field. Decoding a format
does not establish support for writing it, and collection work must not block
the UI thread.

## Primary users and jobs

A listener needs to connect to their engines, browse, manage lists and the
queue, and choose where it plays -- from the desktop, the phone or a script.
Each client must keep up when another changes something.

Someone maintaining a collection needs to listen to new files, identify an
album, edit tags and covers, scan ReplayGain, and rename or convert the files.
Identifiers and unrelated metadata must survive that work.

Someone converting music needs reusable codec and naming presets, resampling,
clear metadata handling, output verification, and eventually DSP and fresh
output loudness values.

## First public releases

The first releases need reliable engine connections, browse and search,
lists, queue editing, transport and output choice. Playback must be gapless,
and tagging, MusicBrainz matching, ReplayGain and conversion must be
dependable with their documented formats.

Plugins, an MPD bridge and a terminal client are not prerequisites for
releasing the player and file tools.

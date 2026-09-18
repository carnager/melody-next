# Playback, server library, working lists, conversion, and verification

ADR-0058 unifies the authorities in Trackbench. ADR-0115 adds the optional
[local library](local-library.md): chosen folders, background indexing,
artist/album browsing, album and track search, and retained unavailable files.
Older descriptions of a permanent process split below are historical.

## Application scope

Since ADR-0025 the repository builds two applications. Server playback and the
server library belong to **Trackknife**, the MPD/Melody client: transport,
outputs, browsing, search, live queue, and stored playlists. Local playback,
lists over local files, and conversion belong to **Trackbench**, the local
file workstation. Sections below name the owning application; shared engine
behavior (decode, gapless, DSP, PipeWire) lives in internal libraries consumed
by Trackbench's player and converter.

## Separate MPD playback and local playback

Trackknife controls MPD as shared server state; Trackbench plays local files
through FFmpeg and PipeWire. The two never merge into one queue, transport
timeline, or backend-neutral progression coordinator. MPD may be controlled by
multiple clients and Melody may expose multiple server-managed agents; a
private local queue cannot honestly own or interleave that state. The process
boundary enforces the separation (ADR-0025), so inside Trackbench local
playback is first-class — its own transport and real list progression — not an
audition mode.

MPD playback remains server-owned: Trackknife sends transport/queue/output
commands and projects server status. Trackbench's local pipeline is:

The local player consumes a logical source reference, resolves it to a physical
source plus subsong/segment, decodes to PCM, applies optional gain/DSP, converts
to the output format, buffers, and writes to PipeWire.

```text
source -> demux/decode -> segment + gapless trim -> ReplayGain -> DSP graph
       -> output sample conversion -> audio device
```

The clean path with ReplayGain/DSP disabled must avoid unintended resampling,
channel mixing, dithering, or level changes. “Bit-perfect” is a property of the
entire configured path and device, not a marketing toggle.

### Required behavior

Trackknife's side is remote MPD transport and output control without local
decoding, projecting the authoritative live queue behind a transport that
controls MPD alone. Everything below belongs to Trackbench's local player:

- play, pause, stop, seek, previous, next;
- gapless transition when codec/container delay and padding are known;
- correct cue/chapter boundaries;
- configurable output device and buffer;
- volume and mute distinct from ReplayGain preamp;
- resume state on restart as an option;
- stop after current;
- cursor-follows-playback and playback-follows-selection as separate options;
- playback orders: default, repeat track/list, random/shuffle track, album, and
  folder groups;
- robust behavior when a file moves, disappears, or changes during playback.

Trackbench speaks no MPD protocol, so local playback structurally cannot
mutate an MPD queue or transport; no UI rule is needed to promise that.

High buffer sizes resist I/O/CPU stalls but increase control/DSP latency; low
sizes improve responsiveness but risk underruns. Expose understandable presets
plus advanced exact values and underrun diagnostics.

The local playback core separates a non-real-time decoder producer from one
real-time PCM consumer through an explicitly sized SPSC ring. The render path
may copy preallocated PCM, zero-fill unavailable frames, and update atomics; it
must not allocate, lock, decode, perform I/O, or notify the UI. Starvation
returns playback to buffering and increments an underrun counter without
advancing the consumed source-sample position. PipeWire must quiesce that
consumer before stop/seek resets the ring.

The first PipeWire adapter connects inactive and negotiates the decoder's exact
source rate as interleaved native-endian float PCM. Mono and stereo have
explicit channel positions; wider layouts are rejected until the core carries
a typed channel map. The real-time process callback only dequeues a mapped
buffer, clamps PipeWire's requested frame count to its capacity, calls the
bounded render function, and queues valid PCM (including zero-filled silence).
Activation waits for streaming. Quiescing deactivates, waits for paused,
flushes queued converter/device data, and waits for an in-flight callback to
leave the source. End-of-source drain separately waits for PipeWire's drained
event. ADR-0021's dedicated player worker — its contract carries over intact
into Trackbench (ADR-0025) — schedules bounded decoder production and
serializes source replacement, transport, seek, PipeWire transitions, drain,
and cancellation. Buffer capacity and start threshold are expressed as
durations and converted to frames at the decoded source rate. Its immutable
snapshot feeds Trackbench's transport row, with PipeWire node and underrun
diagnostics in tooltips.
ADR-0029 adds persistent Responsive (250/50 ms), Balanced (750/100 ms), and
Resilient (2,000/250 ms) ring profiles plus bounded exact custom durations.
Snapshots distinguish the configured policy from the immutable active ring; a
mid-track change applies at the next ordinary load and is visible in the audio
tooltip alongside underruns. These are decoded-PCM safety-buffer policies, not
claims about end-to-end device latency or forced PipeWire graph quantum.
ADR-0030 adds a persistent PipeWire registry plus default-metadata monitor.
System-default streams stay under session-manager dynamic relinking; removal of
an explicitly selected sink pauses without fallback, retains sample position,
and reconnects paused when the node returns. Device-clock projection and
representative latency/underrun measurements remain.

### Gapless details

Do not implement “gapless” by trimming detected silence. Honor codec/container
sample counts, encoder delay, padding, and segment boundaries. LAME/iTunes MP3
gapless metadata, MP4 gapless data, and inherently sample-exact lossless/Vorbis/
Opus paths need fixtures. A separate silence-removal DSP is optional and must be
off by default because it changes artistic timing.

The initial decoder-core acceptance set is PCM WAV, AAC in M4A, LAME MP3, and
Opus in Ogg. The decoder core requests FFmpeg's explicit skip-sample metadata
and performs delay/padding trim itself. Seeking decodes a conservative
one-second preroll, or a larger codec/container requirement when reported,
before trimming to the exact logical sample; this also restores bounded codec
convergence such as an MP3 bit reservoir. Decoded PCM after the first timeline
anchor is counted contiguously rather than independently rescaled from every
frame timestamp; this prevents coarse container time bases from drifting across
sample-exact cue/chapter ranges.

ADR-0031 admits container chapters only when they form an exhaustive adjacent
partition of the known selected-audio duration. Trackbench then projects them
to the same logical identity, physical source, and end-exclusive sample-range
model as external cues. Navigation-only, partial, or malformed chapter tables
remain one ordinary whole-file item.

ADR-0032 adds typed optional audio-stream and codec-subsong selection before
the sample range. Alternate container streams are never expanded implicitly;
language, commentary, and alternate mixes require an explicit future choice.
Tracker files use bounded libopenmpt identity enumeration and FFmpeg selected
decode. Each multi-song module becomes finite persisted rows over libopenmpt's
musical durations, excluding the backend-generated fade tail from the clean
path.

### DSP

An ordered graph supports built-ins such as resampler, channel conversion,
equalizer, crossfeed, convolver, compressor, skip silence, and pause-between-
tracks as appropriate. Each processor declares accepted/produced sample formats,
latency, state reset/flush behavior, thread safety, and whether it prevents a
gapless transition. Presets are reusable by playback and conversion, but the
user must see when a conversion makes processing permanent.

## Input and output formats

foobar2000's current native list includes MP1/2/3, M4A with AAC/ALAC, raw AAC,
Vorbis, Opus, Musepack, Speex, AC-3, DTS, WMA, FLAC/Ogg FLAC, WavPack, Monkey's
Audio, TAK, WAV/Wave64/RF64, AIFF, AU/SND, Matroska/WebM, and MPEG transport
streams, with more through FFmpeg/component decoders. Trackbench should aim for
at least that practical breadth on Linux.

The architecture must distinguish container, codec, and logical tracks. Never
infer capability from extension alone. See the multi-dimensional support table
in `feature-matrix.md`.

Internet streams (MP3/AAC/Vorbis/FLAC/Opus and HLS) are later core candidates.
Stream metadata changes over time and must not be treated as stable file tags.

## MPD server library

### Responsibilities

- browse folders, artists, albums, and other advertised tag dimensions through
  bounded MPD commands;
- search and filter on the server rather than copying the database to the UI;
- preserve rich remote references, repeated tags, MusicBrainz identities, and
  server provenance;
- respond incrementally to database/update idle events;
- load cover art/readpicture asynchronously and cache it by server identity;
- keep remote-only items fully functional when no local root mapping exists.

Trackknife's database stores profiles, workspace, lists, snapshots, and
caches; Trackbench's stores its own lists, presets, jobs, and journals.
Neither is a second canonical server-library index. MPD owns its library;
embedded tags remain canonical for local file metadata.

### Optional local music-root mapping

Each connection profile may map relative MPD URIs below one raw local root. The
mapping is containment-checked and revalidated before any use. Its eventual
purpose is opening the explicit local counterpart in Trackbench for tagging,
scanning, organization, or conversion—a cross-tool convenience deferred until
both tools stand alone (ADR-0025). It never creates a Trackknife library tree,
and the client itself offers no local file operations. Remote URLs, traversal,
non-lossless names, and unavailable paths simply remain remote-only.

Local files enter Trackbench through open, drag/drop, command-line arguments,
and working tabs. They can also be reached through its folder browser, which
lazily browses explicitly selected filesystem roots rather than importing them
into a collection database. Local metadata reads are job-backed, bounded, and
cached only as needed by current views, lists, and operations. Users may also
opt into the separately configured ADR-0115 local library; ordinary folder
browsing never enrolls a directory in that index automatically.

### Metadata cache and large fields

Each application caches normalized values plus native/provenance data needed
for its list display—Trackknife for remote items, Trackbench for local
files—with large lyrics/artwork stored separately and loaded lazily.
Never replace a large value with an ambiguous dot/placeholder in an editor; show
an explicit unloaded state and fetch it before permitting a destructive write.

## Browsing and views

Trackknife ships a coherent default layout rather than demanding UI
construction:

- connection-aware MPD navigation/sidebar and search;
- fast track table with configurable title-formatted columns and grouping;
- artwork/now-playing area;
- transport and queue access;
- job center and command palette.

Trackbench reuses the same track table, transport row, job center, and command
palette components over its tabbed local lists; the two applications share one
visual and interaction language (ADR-0025).

Advanced users can define columns (name, compatible format expression,
alignment, width, sort expression) and grouping. Faceted views allow chains such
as Genre -> Artist -> Album and refine the result set at each pane. Multi-value
fields appear in each appropriate facet without flattening.

Library-tree definitions store an explicit ordered level list; rendered text is
never split to infer structure. Each level has independent grouping, label, and
stable-sort `tkfmt-1` expressions plus an optional singleton-elision policy.
The root also declares the MPD tag used for lazy exact branch queries.
Multi-value expansion uses `$each` and its bounded tree-host semantics; ordinary
`%field%` rendering never creates branches accidentally.

## Working lists and stored playlists

In either application, a scratch/named list is an ordered list of item
references, not just paths:

```text
PlaylistItem
  stable track ID when known
  source URI + subsong/segment identity
  optional cached TrackRef snapshot
  per-item flags/state
```

The cached snapshot can preserve display metadata, duration, and ReplayGain for
offline/missing sources, echoing the useful property of foobar2000 FPL. It is a
cache with provenance, not canonical metadata.

Support tab create, rename, pin, duplicate, reorder, add/remove, undo, search
within, deduplicate, crop selection, sort by title format, reverse, randomize,
and total duration/size. Local list modifications persist transactionally
without requiring app shutdown.

ADR-0123 implements bounded session undo/redo for local removal and rearrangement,
with fresh metadata/path reconciliation for removed occurrences. Add/replace,
cross-tab, and automatic structural changes clear affected history in this slice.
The resulting list persists, but undo history does not survive restart.

ADR-0127 adds whole-list **Sort list**, **Reverse list**, and **Remove duplicate
entries** in Edit and local track menus. Sorting has title, artist/album/track,
album/track, number, and path presets, plus an inline custom `tkfmt-1` expression
and ascending/descending choice. It compares digit sequences numerically (2
before 10), ignores simple Unicode case differences, and preserves equal-key
order. Duplicate removal keeps the first exact raw-path/logical-reference/
decoder-selection/sample-range occurrence; distinct logical tracks stay separate.
Each command is cancellable during preparation and creates one named undo step.
Sort/reverse retain the selected and playing occurrences; duplicate removal uses
the ordinary removal behavior for a removed playing occurrence. Changes persist
through the existing workspace save. These commands never mutate files.


ADR-0125 adds find within the active local list or MPD queue through Ctrl+F,
Next/Previous, and F3/Shift+F3. Literal case-insensitive matching covers cached
title, artist, album, album artist, date, track number, and escaped local paths
or exact MPD URIs. Matches select existing occurrences without changing list
order or playback; Escape closes find. Traversal is bounded and cancellable,
with stale results rejected after edits, server queue refreshes, or tab changes.
Finding never sends MPD commands.

MPD stored playlists remain server-owned and capability-driven. Loading a stored
playlist into the live queue is not the same operation as opening it as a
working tab. The UI makes destructive server changes and local tab changes
distinguishable.

### Import/export

**Trackknife decision — implemented M3U8 profile (ADR-0128):** File → Import
M3U8 playlist creates a new local list from either authority. Relative paths use
the playlist directory; order, duplicates, and unavailable files survive import
and workspace restart. UTF-8/BOM, CRLF, plain file paths, local percent-escaped
file URIs, and EXTINF titles/durations are supported. Embedded tags supply columns
when readable; missing sources retain cached labels. Import does not expand
chapters, subsongs, or nested playlists. Remote URIs, HLS, malformed input, and
unsupported extended directives reject the entire import with a line diagnostic.

File → Export list as M3U8 exports the selected local list's whole-file references,
titles and durations, with relative paths beneath the output directory. Raw names
that cannot safely occupy a line use escaped local file URIs. Logical references,
sample ranges, and explicit stream/subsong selections reject export with a row
number. Other cached metadata and workspace/query/view state are not stored in
the portable file. Export creates a **new file** and never replaces an existing
destination. Both commands offer progress and cancellation, with no partial list
or playlist publication. See ADR-0128 for limits and the publication commit point.

Remaining format direction:

- M3U8 is the minimum portable format; resolve relative paths against the file.
- M3U may require explicit encoding detection/selection.
- XSPF/PLS are useful later interoperable formats.
- Cue sheets create logical subsong items, not ordinary playlist semantics.
- A future FPL importer is best-effort and version-aware because FPL is
  undocumented and intended by foobar2000 as internal, non-interchange storage.

Export warns when the chosen portable format cannot represent subsongs, remote
identifiers, cached metadata, or automatic query behavior.

## Live queue and local playback queues

The live MPD queue is the server's current playlist. Items may occur more than
once and retain stable MPD song IDs; users can add next/end, reorder, remove,
clear, prioritize where supported, and save queue contents. Other MPD clients
may modify it concurrently, so positions are never treated as stable identity.

Trackknife's scratch and named lists hold remote references as working memory,
but they are not playback queues; only the live MPD queue drives server
playback. Trackbench's tabbed lists hold local files and are its playback
queues: its player owns real list progression behind its own transport, and
because Trackbench speaks no MPD protocol it cannot stage MPD items, hand
progression to the server, or touch MPD transport at all (ADR-0025).

## Autoplaylists and saved views

Autoplaylists are live query results described in `query-language.md`. They are
not manually editable. Store query source, dialect version, and sort. Updating
metadata or statistics invalidates affected membership incrementally.

## Playback statistics

Track-owned statistics include play count, first/last played, skip count,
rating, and optional last position. They bind to stable identity/content rather
than path alone, survive Trackbench file operations, and are importable/
exportable. Define when a “play” counts (time/percentage thresholds) and when a
skip counts. Never write statistics into audio tags without explicit opt-in.

Ratings are implemented (ADR-0179): an integer 0-10 shown as five stars in
half-star steps. Local track and album ratings live in the `local_ratings`
table keyed by sha256 content-identity hashes byte-compatible with the Melody
server, so they survive rescans, renames, and moves, and a future server
synchronization is a pure key join. Retagging the identity fields (album
artist, album, title, track number, date) detaches a rating, exactly as it
does in Melody. Ratings are never read from or written to file tags today;
the recorded future path is an explicit opt-in write through the previewed
metadata plan workflow using the established per-container conventions
(ID3v2 `POPM`, Vorbis-family `RATING`).

## Converter

Trackbench's converter accepts selected logical tracks and a named preset:

```text
decode exact logical range
  -> optional ReplayGain application
  -> optional DSP graph
  -> resample/channel/bit-depth conversion + dither policy
  -> encode/mux
  -> transfer mapped metadata/artwork
  -> optional output ReplayGain scan and verification
  -> publish destination atomically
```

### Preset data

- output container and codec;
- encoder implementation/version and quality/bitrate/lossless settings;
- PCM/resample/channel/bit-depth policy;
- ReplayGain source/processing;
- ordered DSP preset;
- shared output-layout and destination-profile references, with optional
  per-conversion-job overrides;
- one-file-per-track, grouped multi-track, or merge mode;
- conflict policy;
- tag/artwork transfer mapping and exclusions;
- cue/chapter creation policy;
- verification and post-scan actions;
- concurrency cap.

Encoder presets are declarative and versioned, and show missing encoders before
starting. ADR-0173 snapshots the complete interactive job beside each saved
encoder preset, including backend versions; human-readable interchange remains
future work. External command-line encoders may be supported through a safe
argument-array template—never shell-string interpolation—and a capability probe.

Per ADR-0054, one-output-per-track conversion reuses Trackbench's versioned
relative-directory/basename output layouts and separate explicit raw-path
destination roots. The converter owns the extension selected by its container
and may override either profile for one job without mutating the shared
default. Grouped and merged outputs remain converter-specific because they do
not necessarily preserve a one-source-item/one-relative-path relationship.

### Destination modes

- One output per logical track.
- Mirror the complete source directory structure relative to `/` (ADR-0154).
- Generate directories and names from a useful preset or `tkfmt-1` expression.
- Grouped multi-track output when the target container supports chapters/tracks;
  otherwise an audio file plus cue where meaningful.
- Merge all selected audio into one continuous track.

ADR-0173 qualifies the first, mirror, and expression modes for M8. Grouped
chapter containers and merge-all output remain future features pending a typed
metadata and boundary contract; CUE/chapter/subsong inputs already produce one
verified file per exact logical selection.

Preview every output path and conflict. Write to temporary siblings and publish
atomically. A cancelled/failed conversion removes only Trackbench-created temp
files, never pre-existing destinations.

### Publishing into an MPD music root

A publication destination may be a directory that happens to be an MPD
`music_root`; to Trackbench, which speaks no MPD protocol, it is plain
filesystem output (ADR-0025). The user selects copy, move, or converted
publication. Trackbench generates and previews contained relative paths,
checks conflicts and source revisions, stages data, verifies every output, and
publishes it atomically. Requesting an explicit MPD database update afterwards,
and adding the published items to a stored playlist or the live queue, is a
job for any MPD client, including Trackknife. Move semantics delete originals
only after verified publication and leave a truthful recovery record on
partial failure.

A transactional Melody upload/import protocol does not currently exist and is
not required by this workflow. If Melody later advertises one, an upload
destination would need protocol awareness Trackbench does not have; like other
cross-tool conveniences it is deferred (ADR-0025), and it must not silently
change the behavior promised for stock MPD.

### Metadata transfer

Map logical fields through target-container rules and show unrepresentable
fields. Copy artwork subject to format limits. Do not transfer ReplayGain when
the signal was changed; scan output instead. Cue-compatible metadata requires a
separate limited mapping.

### Concurrency

Conversion is naturally parallel per independent output but encoders can be CPU
and I/O heavy. Use a bounded scheduler with separate decode/encode resource
weights. Multi-track/group outputs are single dependency chains and maintain
selection order.

## Verification and diagnostics

### Integrity verification

Decode selected logical tracks to completion and report decoder/container
errors. Where available, verify codec-native checksums such as FLAC audio MD5.
Distinguish a missing checksum, a decode error, and a checksum mismatch.

### Audio comparison

Compare decoded PCM under explicit rules:

- exact samples when formats/sample layouts match;
- account for known encoder delay/padding and segment boundaries;
- optionally normalize integer/float representations without resampling;
- report first difference, differing sample count, max absolute error, and
  whether streams are bit-identical or merely equivalent under the chosen rule.

Never call two lossy encodes identical based on metadata/duration alone.

### Decode benchmark

Measure decode speed separately from file I/O where feasible, with worker count,
backend/version, source, and cache state recorded. This is useful for diagnosing
the performance problem Trackbench explicitly intends to avoid.

## Error model

All long operations report structured errors containing operation, track/source,
stage, backend, recoverability, and technical detail. Summaries group recurring
causes but retain individual rows. Logs are exportable with private paths
redacted on request.

## Local playback options

Local tabs show Repeat, Random, Single (`1`), Consume (`C`), and ReplayGain in
the status bar, also available through Playback. These settings are remembered
separately from MPD. Repeat loops the list; Random visits each entry once per
cycle, and Previous retraces the current shuffle cycle.

Single and Consume cycle Off → On → One-shot (`×`). Single stops after the
current track, or repeats it when Repeat is on. Consume removes played entries
from the list and leaves the files on disk; combined with Single, it removes
one entry and stops. One-shot switches itself off after one completion or
removal. Local playback keeps progressing in its original list while another
tab is visible.

ReplayGain offers Off, Track, Album, and Automatic. Automatic uses Track with
Random enabled and Album otherwise. Album falls back to Track when album gain
is missing. Missing gain plays unchanged. Available matching peak metadata
reduces gain to prevent sample clipping; without a peak this protection is not
known. Settings take effect as buffered audio drains. See
[ADR-0119](adr/0119-local-playback-modes-and-replaygain.md) for boundary behavior.

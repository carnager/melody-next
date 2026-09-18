# MPD client, sources, queues, and Melody capabilities

## Scope

This document defines the first visible product slice. Trackknife is a standard
MPD client before it is a Melody-enhanced client. The MPD server remains the
authority for its database, current queue, stored playlists, playback state,
options, mixer, and outputs.

Since ADR-0025 this specification describes only Trackknife, the pure
MPD/Melody client. Local file playback, tagging, and file operations belong to
Trackbench, the separate local workstation; Trackknife's transport controls
MPD alone.

Normative protocol behavior comes from the current [MPD protocol
documentation](https://mpd.readthedocs.io/en/stable/protocol.html). The client
adapter uses the MPD project's [libmpdclient](https://www.musicpd.org/libs/libmpdclient/)
but owns its domain model, retry rules, scheduling, and extension parsing.

`../melody/melodyd/mpd.go` and `../melody/melodyd/mpd_commands.go` are the local
reference for Melody's advertised extensions. Trackknife must not assume those
extensions exist merely because a server happens to report an MPD version used
by Melody.

## Connection profile

```text
MpdProfile
  stable profile ID
  display name
  host or Unix socket
  port (for TCP)
  optional password reference
  connect/command timeout policy
  optional local music root (raw OS path)
  reconnect enabled + bounded backoff policy
```

Credentials are not copied into logs, exported diagnostics, list snapshots, or
formatting contexts. A password embedded in MPD's historical `MPD_HOST` syntax
may be imported, but persisted secrets should use the desktop secret service
when that adapter exists.

One profile is active at a time. The first-run dialog can create and edit
multiple saved profiles, and the Server menu switches among them in one action;
simultaneously active servers remain deferred until the single-session
semantics are proven.

The profile selected for automatic connection is restored at startup. Stable
profile ID, name, host, port, and the raw local music root are stored in the
versioned SQLite state database. The optional password remains session-only;
automatic authenticated reconnect waits for the desktop secret-service adapter
rather than writing credentials to ordinary settings.

## Session architecture

```text
UI commands/events
      |
MpdSession service (generation, state, immutable snapshots)
      |                         |
serialized command worker      idle worker
      |                         |
libmpdclient connection        libmpdclient connection
      \_________________________/
                   MPD server
```

- Neither DNS/connect nor any protocol call runs on the Qt UI thread.
- The command connection serializes requests and complete responses. MPD is a
  stateful line protocol; interleaved requests on one connection are invalid.
- The idle connection waits for selected subsystem changes and schedules small
  refreshes through the command worker.
- Each connection attempt increments a generation. Results from an older
  generation are discarded even if they arrive after reconnect.
- Reconnect uses bounded exponential backoff with jitter and a user-visible
  immediate retry command.
- A disconnect fails waiting commands promptly with structured uncertainty.
- Read-only/idempotent refreshes may be retried in a new generation. Add, move,
  delete, save, and other non-idempotent mutations are never replayed
  automatically after an ambiguous write/response failure.
- Command and idle threads are part of one bounded session resource, not drawn
  from the CPU-heavy scan/conversion pool.

## Capability discovery

After greeting/authentication, collect:

- server protocol version;
- `commands` and, where relevant, `notcommands`;
- enabled `tagtypes`;
- URL handlers/decoders only when their display is useful;
- status, statistics, outputs, and ReplayGain status supported by commands.

The domain snapshot stores the exact advertised command/tag names plus derived
feature flags. UI actions are shown or enabled from capabilities, not from a
hard-coded server version. An `ACK` for an advertised optional command becomes
a visible server/protocol error and may downgrade that session capability.

Track ratings (ADR-0179) follow the same rule: an advertised `sticker`
command enables the interoperable 0-10 `rating` song sticker used by other
MPD clients, loaded in bulk with `sticker find song "" rating` and refreshed
on the sticker idle event. An advertised `getrating` switches the backend to
Melody's native rating extension instead; `albumrate` additionally enables
album ratings. The queue's Rate menu and the optional Rating column stay
hidden or disabled when neither backend is advertised.

Against a Melody server advertising `searchalbums`, the search results'
album section comes from the server's album records — exact identity,
track count, duration, stored and computed ratings painted as yellow stars
over the covers — instead of being derived from returned songs. The Search
dialog additionally gains a **Server library** scope that translates tkq
queries to the server's filter grammar (see
[query-language.md](query-language.md)), with saved searches and committed
`tkq:` result tabs.

Against a Melody server, the library search box also accepts rating terms:
`rating>=8`, `rating==10`, or `albumrating>=6` tokens (operators `>`, `>=`,
`<`, `<=`, `=`) combine with the remaining words into one server-side filter
expression. Without the Melody capability the same tokens search as ordinary
text, so queries degrade instead of failing on stock MPD.

## Remote track projection

```text
RemoteTrackRef
  server profile ID
  MPD URI (exact protocol text)
  optional queue song ID and displayed position
  optional duration/last-modified/range
  ordered repeated metadata pairs
  normalized known-field projection
  MusicBrainz identity projection
  optional lexically mapped local source
  snapshot revision/provenance
```

Repeated fields remain ordered values. Unknown fields remain available even if
the current UI or `libmpdclient` enum does not recognize them. Normalized lookup
is case-insensitive, but original spelling and values are retained in the raw
projection.

Known MusicBrainz fields include artist, album-artist, recording, release-track,
release, release-group, work, and disc identifiers plus artist/album-artist sort
names when the server exposes them. The exact aliases and fallback grouping are
tested in a repository-owned metadata corpus. Missing MusicBrainz data is an
ordinary deterministic fallback case, not an error.

## Optional local music root

The profile's local music root means “this filesystem exposes the same relative
music paths as this MPD server.” It does not configure or scan a Trackknife
library.

Mapping rules:

1. The MPD URI must denote a relative file path, not a URL, absolute path,
   virtual item, or stream.
2. Preserve the URI literally; do not treat `%xx` as URL escaping.
3. Reject NUL, empty path traversal components, and `..` traversal.
4. Join below the configured raw root and verify lexical containment.
5. If conversion from MPD's protocol string to a local OS path is not lossless,
   report “not locally resolved” rather than inventing a path.

Mapping is lexical only; Trackknife never reads or mutates the mapped file.
Opening a mapped server item in Trackbench is deferred cross-tool work
(ADR-0025), and any future file operation must first resolve filesystem
identity safely and reject a symlink/mount race that escapes the root. A
failed mapping only marks the item as not locally resolved; remote browsing
and MPD playback continue normally.

## Live queue

MPD calls its current queue the current playlist. Trackknife calls it the
**live MPD queue** to avoid confusing it with stored playlists and tabbed
working lists.

Every occurrence keeps its MPD song ID. Add, play, seek, move, delete, and
priority operations use ID variants when available. Display positions are
recomputed after every accepted server revision.

Multi-selection append, positional Add next, deletion, and crop preserve display
order and use bounded MPD command lists. They are submitted once: an ambiguous
disconnect reports uncertainty and never replays part or all of the mutation.
Multi-selection drag reorder computes the authoritative target permutation from
stable song IDs and likewise submits one bounded command list; the view waits
for the server snapshot to confirm the new order.

Queue priority is an optional capability-gated action. When `prioid` is
advertised, one or more selected occurrences may be assigned a value from 0 to
255 as a bounded stable-ID command list. The queue snapshot retains MPD's
reported `Prio` value; the UI never invents a local priority state.

Refresh policy:

1. Snapshot `status`, including playlist version and length.
2. If there is no compatible snapshot, fetch the full queue.
3. On a queue idle event, request status and use `plchanges`/`plchangesposid`
   from the known version when supported.
4. Apply a diff only when its base version and shape match the current snapshot.
5. Fall back to a full snapshot on disconnect, version regression, malformed or
   truncated response, inconsistent IDs/positions, or unsupported diffs.

Optimistic UI updates may improve drag feedback, but remain pending until the
server confirms them. A conflict refreshes the authoritative queue and explains
what changed; it never silently overwrites another client's edit.

When an ID-addressed command is rejected because its occurrence disappeared,
the session classifies the recoverable MPD ACK as a queue conflict, schedules
an immediate queue/player refresh, and reports the reconciliation through a
non-modal toast. The rejected mutation is never retried.

## Queue/list tabs

The central tab strip contains several explicit list kinds from ADR-0010:

- live MPD queue;
- MPD stored playlists;
- unsaved/persistent scratch lists;
- named Trackknife lists.

All kinds preserve duplicates and order and use the same virtualized track
view. Their supported mutations and save targets differ. Tabs hold server
items only: tags, ReplayGain, renaming, and conversion are Trackbench work,
and handing a mapped tab or selection to Trackbench is deferred cross-tool
work (ADR-0025). There is no mixed playback-queue tab.

Scratch and named Trackknife lists store rich snapshots so remote entries stay
legible while disconnected. Snapshot metadata is fallback display data, not
canonical server or file metadata.

## Server library and search

Per ADRs 0120 and 0122, the Sources panel hosts the MPD search field and either
the browse tree or an expandable Albums/Tracks tree matching local search.
Clearing search restores browsing;
focus changes retain results, and switching authority preserves the query.
The search uses the existing bounded server service and queue actions. Album
expansion fetches the complete release asynchronously. Multi-selection actions
and drops deduplicate overlapping albums/tracks and retain MPD authority.

ADR-0126 propagates browse-model resets, including database/update notifications
and reconnect reloads, to retained search artwork. Search requests fresh covers
and rejects obsolete artwork responses while preserving its query and result
rows.

Initial browsing is server-backed:

- artist/album/folder navigation through `list`, `lsinfo`, `find`, and related
  advertised commands;
- server-side exact filtering and search rather than downloading the complete
  database to filter on the UI thread;
- `window` pagination where supported, with bounded fallback requests;
- album art/readpicture fetched asynchronously in bounded chunks, advancing
  offsets until MPD explicitly returns an empty chunk rather than mistaking a
  server-selected short binary response for end-of-file;
- database/update idle events invalidate only affected views where possible.

The left Server tab uses the configurable hierarchy accepted in ADR-0019. It
lists only the configured root tag initially and issues one bounded exact tag
lookup when that branch expands. Descendants are grouped from that branch
snapshot using the definition's `tkfmt-1` expressions. The default is album
artist → album in ascending date order with undated releases last → disc only
for multi-disc releases → tracks. A branch above 10,000 tracks is reported as a
limit error rather than shown as complete after truncation.

Trackknife may cache pages, artwork, and projections for responsiveness and
offline tab display, but this cache is not a competing local music library.

## Transport, options, and outputs

Status and now-playing snapshots include play/pause/stop, elapsed/duration,
volume, queue occurrence, next occurrence, repeat/random/single/consume,
crossfade when supported, audio format, errors, and update state.

Output records retain standard ID/name/enabled/plugin/attributes. Standard MPD
allows multiple outputs to be enabled; the UI must not force radio-button
semantics.

Melody currently adds:

- `outputprimary`;
- `outputonline`;
- `outputformat`;
- `outputmaxbitrate`;
- `switchoutput`, an exclusive handoff command.

These values are extension metadata. Offline does not mean disabled: Melody
persists enabled outputs while an agent is disconnected. Additive enable/
disable remains the default interaction; “switch exclusively” is offered only
when the command is advertised.

When `melody_version` is advertised, Trackbench also starts its protocol-v2
playback endpoint for the active profile (ADR-0174). Melody accepts
`agent_register` as a special connection takeover but does not list it in the
ordinary MPD `commands` response. A secondary connection
fetches a playlist-version-consistent queue; the server remains authoritative
for transport, current/next positions, and the shared clock. The endpoint uses
the profile's contained local-root mapping when possible and otherwise streams
by stable Melody song ID. It is not started for stock MPD.

## Tests

The MPD test suite needs:

- a scriptable fake server for fragmented lines, delays, malformed pairs,
  greetings, `ACK`, disconnect points, reconnect generations, and idle races;
- golden response fixtures for repeated/unknown/MusicBrainz tags;
- queue diff, concurrent edit, duplicate occurrence, and ambiguous mutation
  tests;
- URI-to-local-root traversal, URL, and non-lossless mapping tests;
- stock MPD integration tests for every advertised first-slice command;
- Melody integration tests for its shared command surface and output extension
  fields, plus endpoint registration, queue synchronization, source selection,
  clock/advance, preload, and reconnect fixtures derived from `../melody`.

No test may require a developer's personal music library or a live network
service.

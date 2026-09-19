# ADR-0187: Server lists are stored playlists, played as contexts

## Status

Accepted, 2026-09-19. Corrected by ADR-0188, which restores client-owned
working tabs alongside stored playlists — the two have different jobs.
Everything below about stored playlists as playable contexts stands.
Supersedes ADR-0181 (client-owned server list tabs);
extends ADR-0129 (stored playlist tabs) and ADR-0058.

## Context

Trackknife grew three list concepts: local working lists, the client-owned
"server lists" of ADR-0181, and MPD stored playlists. The middle kind
duplicated the last without being visible to any other client, and the two
behaved differently — server lists opened tabs and were freely editable,
stored playlists lived in the sidebar and round-tripped every edit. Users
reasonably asked why there are three.

The deeper limitation was playback: MPD has one queue, so "play this list"
meant overwriting whatever was playing. That is why client-owned lists
existed at all — they could at least be arranged before being dumped into
the queue.

Melody's playback contexts remove that limitation: a stored playlist can be
materialized into the queue while the displaced queue is stashed and every
playlist keeps its own resume point.

## Decision

**A server list is an MPD stored playlist.** The client-owned kind is
retired.

- Stored playlist tabs are the server list tabs. Enter or double-click
  plays the list from that row via `melody_context play <name> <row>`, so
  switching between lists resumes each one and destroys nothing. Without
  the extension the same gesture replaces the queue and plays from the row
  — the gesture means the same thing on stock MPD, minus the memory.
- The tab whose list is the active context marks the playing row. Playlist
  rows carry no queue id, so the marker addresses the row position from
  `status`; other tabs clear it.
- "Copy to server list" creates or extends a real stored playlist and opens
  its tab, so a list made here is the same object every client sees.
- Edits stay server round trips (ADR-0129) and now include multi-row
  reorder, walked as a sequence of single moves with the running index
  offset applied.
- Open tabs persist across restarts: only the names are stored, contents
  always come from the authoritative re-read.
- Legacy client-owned documents are not lost. They wait until a connected
  server advertises `playlistadd`, then become stored playlists with
  collision-suffixed names and open as tabs.

## Consequences

- Two list concepts remain, and they mean different things: local lists
  (files on this machine) and server lists (playlists on the server).
- Editing a list never affects another; playing one never destroys
  another's queue. The MPD Queue tab remains the scratch context.
- A server list requires a server, by definition — offline, the legacy
  documents simply wait instead of rendering as tabs.

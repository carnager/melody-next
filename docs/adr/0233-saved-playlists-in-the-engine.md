# ADR-0233: Saved playlists live in the engine

Status: Accepted (2026-09-26)

Amends ADR-0227, which put saved playlists on "the engine that owns their
files" but deferred the move to "the file-work step". File work stays in
Trackknife (ADR-0226), so that step is not coming; this makes the move on its
own.

## Context

A playlist you saved by name exists only in the Trackknife that saved it,
in that computer's `lists.sqlite`. The phone, `melody-cli`, rofi and a second
Trackknife cannot list or play it, even when every track in it is on the
engine they are connected to. Remote tabs make it plain: a saved list of
gemenon's files is stored on caprica.

ADR-0227 already drew the line this ADR keeps: tabs, scratch lists and
search results are one window's workspace and stay in Trackknife. Only saved
playlists move.

Two facts constrain the design:

- On a desktop the engine and Trackknife open the same `lists.sqlite`, and
  Trackknife saves its workspace by replacing `list_documents` wholesale
  (`ListRepository::replace_all`). The engine writes none of those tables
  today. It must not start: two writers of one table, one of them
  delete-and-reinsert, lose updates.
- Moving or renaming files is Trackknife's work, and lists must follow a move
  in one logical transaction (AGENTS.md). Today that is one SQLite
  transaction, because the lists are in the database Trackknife writes. An
  engine's playlists are not.

## Decision

**Ownership.** A saved playlist belongs to one engine: the one whose files it
lists. A list of this computer's files belongs to this computer's engine; a
remote tab's to the remote engine. Lists never mix engines (they do not today
either: a document is remote or not).

**Storage.** New tables in the engine's database, `playlists` and
`playlist_items`, written only by the engine. Not `list_documents`, which stays
Trackknife's workspace. An item is what a list item is now minus the
workspace parts: raw path, entry identity (ADR-0221), segment and stream or
subsong selection, duration, and a small display snapshot (title, artist,
album) given by the client that saved it, for files the library does not
index. Each playlist has a stable id, a name and a revision that changes on
every write.

**Protocol.**

| Method | |
| --- | --- |
| `playlist.list` | id, name, track count, revision, modified time of each |
| `playlist.get {id}` | the items, and the revision |
| `playlist.save {id?, name, items, revision?}` | creates, or replaces the items; with `revision`, refused as `conflict` if it changed meanwhile |
| `playlist.rename {id, name, revision?}` | |
| `playlist.delete {id, revision?}` | |
| `playlist.play {id, entry?}` | replaces the queue with it and plays -- for the phone and the CLI, which hold no list of their own |
| event `playlist.changed {id, revision, deleted?}` | to every client |

**Trackknife.** A saved playlist opened as a tab is a view of the engine's
playlist: the tab records which engine and which id. Editing marks it
unsaved as now; Save writes it back with the revision it was read at. If
someone else saved it meanwhile, Trackknife asks: reload theirs, keep mine
(overwrite), or save mine as a copy. "Save working list" on a scratch tab
creates the playlist on the tab's engine. A `playlist.changed` from another
client refreshes a tab with no unsaved edits, and marks one with edits as
conflicting.

The tab keeps its items in `list_documents` as a cache, so it opens and plays
with its engine away. Saving while the engine is away keeps the tab unsaved,
and says so, until the engine is back.

**Following file moves.** Trackknife's rename/move keeps its journal
(ADR-0044 family). After the files move and the local transaction commits,
it asks each affected engine to relocate: `playlist.relocate {moves:
[{from, to}]}`, applied by the engine in one transaction across its
playlists. The move's journal entry stays open until every engine has
confirmed; an engine that is away is asked again when it returns, from the
journal. One logical transaction, carried by the journal rather than by one
database.

**Migration.** On first connecting to an engine that answers
`playlist.list`, Trackknife offers it the saved lists that belong to it,
keeping their document ids as playlist ids, so doing it twice changes
nothing. A migrated tab becomes a view as above. Nothing is deleted from
`list_documents`; a later release may drop the copies.

**Elsewhere.** `melody-cli playlists` and `melody-cli play playlist NAME`.
The Android app lists and plays them (a follow-up there). M3U8 import and
export stay in Trackknife; an imported list becomes a playlist when saved.

## Not federation

Engines do not merge each other's libraries, playlists or queues. Seeing
several engines is the client's job: it holds a connection to each and shows
their libraries and playlists side by side, grouped by engine (the
"several connections at once" of AGENTS.md). Playing one engine's music on
another machine's speakers is already an output choice (ADR-0228), not a
reason to share catalogues.

Merging would blur who records a listen, a rating or a scrobble -- today
always the engine that has the file -- would half-break a mixed queue when
one engine is away, and would show an album held by two engines twice. If a
queue mixing engines is ever wanted, it can be built on the stream tickets of
ADR-0230 without changing who owns what.

## Not decided here

- Playlists that update themselves (autoplaylists, ADR-0206 live rules). A
  saved search is already engine-side-capable; that is a separate ADR.
- Sharing one playlist across engines, or copying between them beyond "save
  as copy on the other engine".
- Per-playlist view layout on the engine. Column layout stays per window.

## Consequences

- The phone, the CLI and every Trackknife see the same playlists of an
  engine, and a playlist of gemenon's files lives on gemenon.
- A second writer for lists appears -- the engine -- but on tables no one else
  writes.
- Moves span processes and need the journal to finish them; a relocation
  can be pending while an engine is away, and a playlist on that engine
  names the old paths until it returns.

## Verification

- Repository tests: playlist tables, revisions, conflict refusal, relocation
  in one transaction, migration idempotence.
- Protocol tests for every method and the event.
- A window test: save a scratch tab to the remote, edit it from a second
  client, see the conflict offered; move a file and see the remote playlist
  follow, including with the remote away during the move.
- CLI test: list and play a playlist.

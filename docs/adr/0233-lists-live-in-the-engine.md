# ADR-0233: Lists live in the engine

Status: Accepted (2026-09-26)

Supersedes the part of ADR-0227 that kept tabs, scratch lists and search
results in Trackknife, and its deferral of saved playlists to "the file-work
step" -- which is not coming: file work stays in Trackknife (ADR-0226). This
is the move AGENTS.md already names: `ui::ListPersistenceService`, which
holds every list in the UI's own database, "is being moved to the other side
of that line".

## Context

Every list -- a saved playlist, the working list a window opens with, a search
kept as a tab -- exists only in the Trackknife that made it, in that computer's
`lists.sqlite`. The phone, `melody-cli`, rofi and a second Trackknife cannot
see or play any of them, even when every track is on the engine they are
connected to. Remote tabs make it plain: a list of gemenon's files is stored
on caprica.

Each list already belongs to one engine: a document is a remote one or not,
and lists never mix engines. So each list has an obvious home.

Two facts constrain the design:

- On a desktop the engine and Trackknife open the same `lists.sqlite`, and
  Trackknife saves its workspace by replacing `list_documents` wholesale
  (`ListRepository::replace_all`). The engine writes none of those tables
  today. It must not start: two writers of one table, one of them
  delete-and-reinsert, lose updates.
- Moving or renaming files is Trackknife's work, and lists must follow a move
  in one logical transaction (AGENTS.md). Today that is one SQLite
  transaction, because the lists are in the database Trackknife writes. An
  engine's lists are not.

## Decision

**Ownership.** A list belongs to one engine: the one whose files it lists.
A list of this computer's files belongs to this computer's engine; a remote
tab's to the remote engine.

**Working and saved.** A list is a *working* list -- what a window opens
with, a search kept as a tab, anything not yet saved -- or a *saved* one.
Save gives a working list a name and makes it saved; nothing else about it
changes. Closing a working list's tab deletes the list; a saved list stays
until it is deleted. A window showing a list that another deletes closes its
tab.

**What stays in the window.** Which lists a window has open as tabs, their
order, the active one and the column layouts: those are one window's view of
the lists, kept in its settings, not the lists themselves.

**Storage.** New tables in the engine's database, `engine_lists` and
`engine_list_items`, written only by the engine -- not `list_documents`,
which stays Trackknife's until migration empties it. An item is a list item
minus the workspace parts: raw path, entry identity (ADR-0221), logical
reference, segment and stream or subsong selection, duration, and a small
display snapshot (title, artist, album) given by the client, for files the
library does not index. Each list has a stable id, a name, its kind and a
revision that goes up with every write. Access to the engine's database is
serialised, since clients' connections, the recorder and the playback store
all reach it from their own threads.

**Protocol.**

| Method | |
| --- | --- |
| `list.all` | id, name, kind, revision, track count and modified time of each |
| `list.get {id}` | that, and the items |
| `list.save {id?, name, kind?, items, revision?}` | creates, or replaces name, kind and items; with `revision`, refused as `conflict` if the list moved on since (0: if the id is taken) |
| `list.rename {id, name, revision?}` | |
| `list.delete {id, revision?}` | says whether there was one to delete |
| `list.play {id, entry?}` | the list becomes the queue and plays, with the same entry identities -- for the phone and the CLI |
| `list.relocate {moves: [{from, to}]}` | files moved or renamed, followed in every list; says which lists changed |
| event `list.changed {id, revision?, deleted}` | to every client |

**Trackknife.** A tab is a view of an engine's list: it records which engine
and which id. Editing marks it unsaved as now; saving writes it back with the
revision it was read at. If another client saved it meanwhile, Trackknife
asks: reload theirs, keep mine (written regardless), or save mine as a copy.
A `list.changed` from elsewhere refreshes a tab with no unsaved edits, and
marks one with edits as conflicting. The tab keeps its items as a cache, so
it opens and plays with its engine away; writing waits, and says so, until the
engine is back.

**Following file moves.** Trackknife's rename/move keeps its journal
(ADR-0044 family), and the local list update stays in the same transaction
as before. Once a move has committed, Trackknife tells the engines:
`list.relocate {moves: [{from, to}]}`, which each applies in one transaction
across its lists, raising the revision of every list it changed. This
computer's engine gets the paths as they are here; the remote one gets them
through the remote mount, and a file outside the mount is not its to follow.
Each move is kept in Settings until every engine has taken it, so one that
is away is told when it is back -- and the move itself is not reported as
failed for an engine being off. One logical transaction, carried by that
queue rather than by one database.

**Migration.** On first connecting to an engine that answers `list.all`,
Trackknife hands it the lists that belong to it, keeping their document ids
as list ids, so doing it twice changes nothing. Nothing is deleted from
`list_documents` until a later release.

**Elsewhere.** `melody-cli lists` and `melody-cli play list NAME`. The
Android app lists and plays them (a follow-up there). M3U8 import and export
stay in Trackknife; an imported list is a working list until saved.

## Not federation

Engines do not merge each other's libraries, lists or queues. Seeing
several engines is the client's job: it holds a connection to each and shows
their libraries and lists side by side, grouped by engine (the "several
connections at once" of AGENTS.md). Playing one engine's music on another
machine's speakers is already an output choice (ADR-0228), not a reason to
share catalogues.

Merging would blur who records a listen, a rating or a scrobble -- today
always the engine that has the file -- would half-break a mixed queue when
one engine is away, and would show an album held by two engines twice. If a
queue mixing engines is ever wanted, it can be built on the stream tickets of
ADR-0230 without changing who owns what.

## Not decided here

- Lists that update themselves (autoplaylists, ADR-0206 live rules). A saved
  search is already engine-side-capable; that is a separate ADR.
- Copying between engines beyond "save as copy on the other engine".
- Per-list view layout on the engine. Column layout stays per window.

## Consequences

- The phone, the CLI and every Trackknife see the same lists of an engine,
  and a list of gemenon's files lives on gemenon.
- Two Trackknife windows on one engine show the same working lists when they
  open them; closing one's tab deletes it for both.
- A second writer for lists appears -- the engine -- but on tables no one else
  writes.
- Moves span processes; a relocation can be pending while an engine is away,
  and a list on that engine names the old paths until it returns.

## Verification

- Repository tests: the tables, round trip of every field, revisions and
  conflict refusal, working to saved, relocation in one transaction,
  migration idempotence.
- A test against a running engine for every method and the event.
- A window test: save a working tab to the remote, edit it from a second
  client, see the conflict offered; move a file and see the remote list
  follow, including with the remote away during the move.
- CLI test: list and play a list.

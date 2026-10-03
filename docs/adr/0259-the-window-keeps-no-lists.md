# ADR-0259: The window keeps no lists of its own

Status: Accepted (2026-10-03)

Completes ADR-0233 ("step 3d"). Builds on ADR-0256 (lists travel as edits).

## Context

Lists live in their engine (ADR-0233), and every client sees them. Trackknife
still keeps a second copy of every open list in `list_documents`, and that
copy still decides more than it should (measured in the code, 2026-10-03):

- **Tabs come from it at startup.** The window restores its tabs from its
  copy and then reconciles each with its engine; the engine is checked against
  the window, not the other way round.
- **Its rows are richer than the engine's.** An engine list item carries a
  display snapshot -- title, artist, album, album artist, date, length,
  ReplayGain. The window's rows carry every tag with its provenance, and the
  file's revision, which the play-count columns and the revision checks of
  file work rely on. A tab built from `list.get` would show less, and would
  not be read again because it looks read.
- **Some state lives only there:** pinned tabs, and the unsaved edits of a
  saved list, which are deliberately not sent to the engine.
- **Saving it is what sends edits to the engine.** About forty places ask for
  a save, most of them only so that the engine hears of a change.
- **A kept search of the whole library** is 66,000 rows made by the engine,
  sent to the window, and sent back to the engine as a list.

Two lists of one thing, one of them authoritative in practice, is what
ADR-0220 exists to end.

## Decision

**A tab is an engine's list and the window's view of it, nothing more.**

1. **The engine describes its items.** `list.describe {id, entries}`
   answers, for at most 2,000 of a list's entries, what the engine's library
   knows of each file -- every tag, the technical facts and the revision --
   and nothing for a file the library does not index, whose item keeps the
   snapshot it was saved with. A tab opens on its items' snapshots at once
   and asks for the rows that have no revision yet, top first, a part at a
   time, each row filled in where it is. A row so filled is as complete as
   one the window read itself, with field names as the library folds them,
   as a search's rows have. An engine that does not know `list.describe`
   leaves the rows as saved.
2. **The window keeps what is its own in its settings:** which lists are open
   as tabs and on which engine, their order, the active tab, which are pinned,
   and the column layouts. Not in a database.
3. **The window keeps a cache, not a copy.** Each open tab's rows are written
   -- off the window's thread -- to a file of the window's cache directory,
   with the engine, the list id and the revision they are of. At startup the
   tabs open from it at once; once the engine answers, the engine's list
   replaces it. While the engine is away the tab shows its cached rows, read
   only, and says so: playing and editing wait for the engine. The cache is
   never sent to an engine and never merged with one: losing it costs only a
   blank tab until the engine is back.
4. **An unsaved edit of a saved list is a draft on the engine:** a working
   list naming the saved list it is a draft of. Saving writes the draft's
   items to the saved list, against the revision the draft started from (the
   conflict choices of ADR-0233 stand), and the draft goes. Discarding just
   deletes it. Drafts are lists like any other, so a draft survives a restart
   and another client sees that the list is being edited.
5. **Edits reach the engine because they are edits**, not because the window
   saved. A tab's model reports its changes to the sync, which sends them as
   ADR-0256 describes, coalesced; nothing writes `list_documents` any more.
6. **A kept search is made on the engine.** `list.from_query {query, name}`
   makes a working list of a query's matches, in the engine, from its library;
   the window opens it like any list. The 66,000 rows travel once, to be
   shown, and not back.

**Migration.** The first start of a window with this change reads
`list_documents` one last time: the open tabs, their order and pinned flags go
to settings, and any list the engine does not hold yet is handed to it as
ADR-0233's migration does. After that the window neither reads nor writes
those tables. They are dropped by a later release, by a reversible migration.

**Done when** the window opens, edits, saves and restores its tabs with
`list_documents` untouched -- checked by a test that makes the table
unreadable -- and a tab whose engine is away shows its cached rows.

## Progress

- Step 1, the engine describes its items: done (2026-10-03), first as
  `list.get {describe}`, replaced the same day, before any other client used
  it, by `list.describe` in parts once a whole description proved heavy
  (below).
- Step 2, drafts on the engine: done (2026-10-03). `list.draft {of}` answers
  the saved list's draft, made if there is none; `list.commit {id, force?}`
  writes it into the saved list and deletes it, and a queue played from the
  draft is played from the list from then on. Summaries carry `draft_of` and
  `draft_base`. Schema 51. The window does not use them yet.
- Step 3, `list.from_query {query, name, words?}`: done (2026-10-03). The
  window does not use it yet.
- Steps 2 and 3 of the window, its tabs in settings and a cache: done
  (2026-10-03). The window keeps which lists are open -- engine, name, kind,
  pinned, unsaved, layout -- their order and the shown one under `tabs/v1`,
  and each tab's rows in `<cache>/tabs/<id>.tabcache`, written off its
  thread when they changed. The first start reads `list_documents` once and
  keeps its lists so; after that neither it nor the file work's
  bookkeeping of it is touched. A tab whose cache is gone opens empty and
  not edited, and is its engine's list once that answers. Measured on a
  copy of the window's database from before the 2026-10-02 cleanup (debug
  build): a kept search of 66,841 rows is a 103 MB cache, written in 0.7 s
  and read in 1.0 s, against 3.6 s for all of `load_all`. Its size is the
  full tags of every row; compressing it is left for when it matters.
- Not yet: a tab whose engine is away is still editable, its edits sent
  when the engine is back, as before; drafts (step 2) and
  `list.from_query` (step 3) are not used by the window yet.
- Measured on a copy of gemenon's library (66,841 tracks, debug build,
  2026-10-03): `list.from_query ALL` 2.4 s; `list.get` of it 1.6 s for
  24 MB; described whole 9.3 s for 99 MB; described in parts of 2,000, the
  first in 0.25 s and all in 8.1 s, behind a tab already shown.

## Not decided here

- **Paging a long list's entries.** A tab holds every entry of its list, as
  now -- Qt's models need the count, and `list.get` of 66,000 entries is
  1.6 s -- and only the descriptions come in parts. Holding only what is on
  screen would be its own decision.
- **The window's other data in `lists.sqlite`** -- naming layouts, encoder
  presets, transformation chains, the workspace backup -- is not lists, and
  moves on its own.

## Consequences

- One copy of each list: the engine's. A second window, the phone and the CLI
  see a draft as soon as it exists.
- Tabs open as fast as before, from the cache, and are complete once the engine
  answers.
- The window's database writes on every change stop; the sync sends edits as
  they happen.
- Pinned and layout move from the database to settings once, at migration.

## Verification

- An engine test: `list.describe` describes an indexed file from the
  library, revision included, and none outside it, in the order asked, at
  most 2,000 at a time.
- Drafts: a draft of a saved list is a working list; saving writes it with the
  revision it started from, refuses a moved-on list as `conflict`, and deletes
  the draft; discarding deletes it.
- `list.from_query` makes the list of a query's matches.
- Window tests: tabs, order, active tab and pinned survive a restart from
  settings; with the engine away a tab opens from its cache, read only; an
  edit reaches the engine without a save; `list_documents` is not touched.
- Migration: an existing workspace's tabs come back as they were, once.

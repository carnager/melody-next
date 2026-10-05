# ADR-0254: Library views are levels of tkfmt-1 expressions

## Status

Accepted, 2026-10-02. Builds on ADR-0008 (tkfmt-1), ADR-0150 (tkq-1 filters)
and ADR-0220 (the engine answers). Fills "custom library-tree expressions"
in the feature matrix.

## Context

The library tree is album artist › album › track, built by fixed SQL with
three shipped tkfmt-1 labels. A foobar2000 user expects to group the library
any way they like -- by genre, by decade, by label -- and `tkfmt-1` already
reserves `$each(name)` for exactly this: a tree level evaluated once per
value of a multi-value field.

## Decision

- **A view is a name and an ordered list of levels**; each level is a
  `tkfmt-1` expression compiled in the `tree_level` host, optionally with its
  own sort expression and a descending flag. Tracks are always the last
  level. `$each(genre)` puts a track under each of its genres; plain
  `%genre%` keeps its usual single value. No language change.
- **The engine groups.** `LibraryQuery` gains the view's levels, the labels
  chosen so far (`view_path`) and an optional `tkq-1` filter, and
  `catalogue.query` and `catalogue.paths` answer them like any other browse,
  local or remote. One pass over the library reads every track with its
  fields, evaluates the levels, and keeps the grouped tree in memory, keyed by
  the levels, the filter and the library revision, so opening a node
  evaluates nothing. A few recent trees are kept.
- **Entries.** A level's node is a new `group` entry: its label, track,
  album and availability counts, and its path. A node whose tracks are all
  one album, at the last level, is an ordinary album entry instead -- its
  cover, rating, Go to album and drag work unchanged. Under it come its
  tracks, in album order.
- **Order.** Nodes sort by their sort expression, or else their label,
  ignoring case and comparing digit runs as numbers, optionally reversed;
  empty labels last, shown as "Unknown". A node's sort key is the first of
  its tracks' in that order.
- **The default view is the tree as it is.** Artist › Album keeps its own
  queries and is unchanged. Shipped alongside: Genre › Artist › Album,
  Year › Album (newest first), Decade › Artist › Album, Album, and the
  existing Recently added.
- **Views live in the window's settings** (`library/views`, each level with
  its dialect and version), and are sent with each request; the engine stores
  nothing. A dropdown above the library's search chooses one -- Recently
  added, until now a footer toggle, is one of its choices -- and an editor
  makes, changes and removes them, with a live preview. The shipped views can
  be copied, not changed. Go to album and Go to artist return to the artist
  tree, as they already left Recently added. *(Superseded by ADR-0273:
  they find the file in the layout shown.)*
- **Folders is a view too.** The engine's index knows every track's folder,
  so the dropdown offers the library's folders from its roots down, read
  from the index of whichever engine is shown -- the only way to browse a
  remote engine's folders, which the Sources panel's Folders tab (this
  computer's file system) cannot. A folder of one album with no folders
  below is that album's entry; a folder's files are everything below it.
- **Every shipped view is in the editor**, the artist tree and Recently
  added included, each with the levels that group the same way (Recently
  added sorts by `$info(albumdayssinceadded)`), so a copy starts from it --
  the artist tree with years before its albums, say. The shipped trees are
  still shown by their own queries. Folders has no levels and cannot be
  copied. Previewing a view in the editor never chooses it.
- **Searching.** A word search shows its results as today. A `tkq-1` query
  in query mode narrows the chosen view before it is grouped.

## Consequences

- Grouping costs one pass per library revision and view; the tree for
  100,000 tracks is tens of megabytes at most, and is dropped as the library
  changes.
- An engine older than this answers a view's request as the artist level.
- A folder hierarchy is not expressible as levels -- tkfmt-1 has no path
  functions and a tree's depth varies -- so Folders is a view of its own,
  not levels.

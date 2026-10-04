# ADR-0268: What shows a library follows its changes

## Status

Accepted, 2026-10-04.

## Context

The library panel learned of a change only when Trackknife made it through
this computer's engine, and then rebuilt itself: the tree emptied and was
asked again from the top, closing what was open and losing the scroll. A
change anywhere else -- a tag write or a cover saved on another engine, an
album deleted on the NAS that `melody-watch` reported, a scan -- showed only
when the person reloaded. A cover changed nowhere at all: it is no track,
so no row changed.

## Decision

- **The engine says what changed.** Every change to its library -- a refresh
  of paths (a write, a move, a deletion, a watcher's report), a scan, a
  folder added or removed -- is told to every client as `catalogue.changed`
  `{paths, albums, everything}`. `albums` are the album keys the change
  touched: a track's album before and after (a write may move it), every
  album under a folder that went, and for a cover or other file beside
  tracks, the albums of its folder (`albums_touching`).
- **Each engine's panel follows its own engine**, local or remote.
- **In place.** The panel asks again only the levels it has loaded and
  merges each answer into the rows there are, by entry: a row that stays is
  the same row, said again (counts, availability); a new one is inserted
  where it belongs; one gone is removed. What is open stays open, the current
  row and the scroll stay. A query's results are asked again as a search.
- **Covers by album.** The covers of the albums named are let go and asked
  again when shown; after a scan, all of them.

## Consequences

- Protocol level 4. An older engine says nothing; its panel follows only
  what this window does, as before.
- A change that touches many albums asks every loaded level again once
  (changes within a quarter second are taken together).

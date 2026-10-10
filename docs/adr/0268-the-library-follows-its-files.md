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
- **Lists say what is gone.** A list's rows whose files are not there show
  greyed and in italics, their tooltip saying so. Its engine is asked
  (`files.missing`, on the machine where the files are) when the list gains
  rows -- opened, filled, adopted -- and, for the rows at the paths a
  `catalogue.changed` names or under them, again: a row whose file came back
  is shown as any other. The rows stay; what to do with them is the
  person's.

## Consequences

- Protocol level 4. An older engine says nothing; its panel follows only
  what this window does, as before.
- A change that touches many albums asks every loaded level again once
  (changes within a quarter second are taken together).

## Amendment, 2026-10-11: lists follow what their files hold

A file rewritten by another tagger -- or another window -- went on showing
its old tags in every list, and the tag editor, trusting a row it had
written itself, took the row's old file for the file there now: every Apply
was refused, "the source changed after the metadata draft was captured",
until the track was added to the list again.

- **Lists follow tags, not only presence.** The rows a `catalogue.changed`
  names -- at its paths, or under them; all of them after a scan -- are
  described again by their engine (`list.describe`, ADR-0259), in place:
  their tags, length and file identity, their place, entry and selection
  kept. A CUE track is described by its sheet and is left alone.
- **The tag editor reads every file when it opens**, whatever the row knows
  of it, showing the row's tags meanwhile (ADR-0257). A row's identity says
  which file it is, never that its tags are the file's.
- **A file changed between reading and Apply is read again**, the draft
  carried onto what it now holds, with "Changed since read · read again,
  N drafts kept · check, then apply again" -- not a list of problems to
  fix by hand. Anything else blocking Apply is shown as before.
- **A rename follows a row already described again.** The engine's notice
  of a write can reach the window before the write's own result; a row
  described in between names the file as written, and is moved with it as
  one naming the file as it was.

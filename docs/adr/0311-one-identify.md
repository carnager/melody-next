# ADR-0311: One Identify, for one album or many

## Status

Point 1 accepted and implemented, 2026-10-24; points 2 to 4 remain a
proposal. Would in the end supersede the window of ADR-0261 (Identify
albums…), keeping its grouping, lookup and write.

## Context

Melody has two ways to identify albums, which work differently:

- **Identify** (the tag editor, ADR-0090, ADR-0162): one album, and the
  person decides -- the search terms are theirs to change, every candidate
  is shown, every pairing confirmed. Since ADR-0310 it takes a release's
  link or id. It treats whatever is chosen as one album: two albums are
  matched to one release.
- **Identify albums…** (ADR-0261): many albums, and it decides -- grouped
  by itself, looked up in the background from the tags, clear matches
  staged without asking; only albums with several fitting versions reach
  the person. Its search terms cannot be changed, and an album it found
  nothing for, or matched clearly, cannot be opened in its review.

Both share the lookup, the matcher and the proposals; the search, the
fingerprint voting and the candidate rows are written twice. The person
prefers Identify's flexibility, and needs Identify albums' reach.

## Decision

Identify grows into the one tool, keeping its page for each album:

1. **Many albums, one at a time.** With several albums chosen, Identify
   groups them as Identify albums does (`album_groups`: release id, else
   album artist, album and year, else folder) and steps through them: a
   list of the albums and their state beside the page of the one shown --
   "Album 3 of 12", Previous and Next, Alt+Page Up and Down -- each with
   its own search terms, versions and matcher. Each album is looked up as
   it is shown -- its tagged release (ADR-0310), else a search by its tags;
   one of untagged files is the person's to search or fingerprint -- and
   those after it in the background, one at a time beside it, so the next
   is there when reached. One album is the same page alone, now searched
   as it opens.
   Each one accepted is staged into the tag editor's draft, one after
   another, and the next not yet accepted is shown; when every album is
   staged the window closes. The tag editor writes them together, with its
   Actions -- rename, move, ReplayGain -- and its undo (ADR-0262,
   ADR-0263). An album accepted is staged even when the window is closed
   before the tagger takes it.

## Proposal

2. **Clear matches by themselves, if asked.** A checkbox, off by default:
   "Accept clear matches by themselves" -- ADR-0261's clear match. Ticked,
   those are staged without stopping and Next goes to the next album that
   needs the person; each stays open to be looked at and changed.
3. **The grouping beside it.** A compact list of the albums -- their state,
   Split by folder, Merge, Leave out -- next to the album shown, from
   Identify albums.
4. **Identify albums… goes.** Tools › Identify albums… opens Identify on
   the selection, or the whole list.

## Unknown

- Whether Identify then lives in the tag editor's window, or the tag
  editor opens it as now with the list beside it.
- How far a thousand-album batch still needs Identify albums' summary page,
  its filters by state, rather than the list of point 3.

## Verification

- `bench-main-window`: two albums chosen, one tagged with its release and
  one with only its tags, listed; the second looked up while the first is
  shown, by one search; the first accepted and staged, the second shown
  and accepted; both staged with their own releases, and the window closed.
  One album, tagged, searched as it opens.

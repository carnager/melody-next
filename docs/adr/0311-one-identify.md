# ADR-0311: One Identify, for one album or many

## Status

Points 1 and 4 accepted and implemented, 2026-10-24; points 2 and 3 remain
a proposal. Supersedes the window of ADR-0261 (Identify albums…) and its
own write (ADR-0262, ADR-0263), keeping its grouping.

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
   An album's page is its versions and, beneath them, how its files pair
   with the version chosen -- previewed as it is chosen, the best one at
   first, changed by choosing another or dragging files. The version
   previewed is the album's choice; there is nothing to confirm. **Skip**
   leaves an album out, **Don't skip** takes it back. **Apply** stages
   every album previewed and not skipped into the tag editor's draft, one
   after another, and closes the window once they are; an album never
   shown is not applied, and Close applies nothing. One album is the same
   page, applied by Apply. The tag editor writes them together, with its
   Actions -- rename, move, ReplayGain -- and its undo (ADR-0262,
   ADR-0263). Albums applied are staged even when the window is closed
   before the tagger takes them.

## Proposal

2. **Clear matches by themselves, if asked.** A checkbox, off by default:
   "Accept clear matches by themselves" -- ADR-0261's clear match. Ticked,
   those are staged without stopping and Next goes to the next album that
   needs the person; each stays open to be looked at and changed.
3. **The grouping beside it.** A compact list of the albums -- their state,
   Split by folder, Merge, Leave out -- next to the album shown, from
   Identify albums.
## Decision, continued

4. **Identify albums… goes.** Its window, its session, its lookup queue and
   its own write are removed, and so is the tag editor's button for it. The
   Edit menu's and a list's Tools entry is **Identify…**: the tag editor on
   the selected tracks -- the whole list when none are -- with Identify
   open in it once the files are read. Writing, renaming, moving and undo
   are the tag editor's Apply, as for any edit. The pure judging of a clear
   match (`album_lookup`) stays, for point 2. Identify opens at most of the
   screen and keeps the size it is left at; an album's versions stand a few
   rows high, its pairing gets the rest.

## Unknown

- How far a thousand-album batch needs Identify albums' summary page, its
  filters by state, rather than the list of point 3.

## Verification

- `bench-main-window`: two albums chosen, one tagged with its release and
  one with only its tags, listed; the second looked up while the first is
  shown, by one search; the first's best version previewed and counted,
  Next previewing the second's, nothing staged; skipped and taken back;
  Apply staging both with their own releases, and the window closed. A
  version chosen previews its pairing, a release looked up already not
  fetched again.
  One album, tagged, searched as it opens.
- `bench-main-window`: Identify… from the Edit menu opens the tag editor on
  the whole list with none selected and on the one selected, Identify open
  in it; there is no Identify albums… entry or button.

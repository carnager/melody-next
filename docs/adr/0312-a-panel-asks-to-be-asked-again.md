# ADR-0312: A panel asks to be asked again

## Status

Accepted, 2026-10-24. Extends what a panel of ADR-0291 answers.

## Context

- **About waited for covers.** Each run fetched the covers of the missing
  albums it had not kept and waited up to six seconds for them before
  answering -- every track, once an artist had a few dozen albums missing,
  for the Cover Art Archive's images are kept on archive.org, which is
  slow. Measured on a cached artist: 6.0 s, all of it the wait; 0.04 s
  without it. A cover still queued when the wait ended was not marked as
  tried, so the next run waited for it again.
- **A panel could not say it had more coming.** It is asked when the track
  changes, when it is shown, on its refresh button, and as a listening
  room changes. An answer that showed what it had at once could not get
  the rest shown without the person pressing refresh.

## Decision

1. **`again_after`**: a panel's answer may say `"again_after": n`, whole
   seconds from 1 to 60. The panel is asked again about the same after so
   long -- if it still shows, and nothing else is being asked -- keeping
   what it shows, and where it was scrolled to, until the answer comes.
   Ten times running at most for one track; a new track, a button pressed
   or a refresh starts again. A value out of place is an answer that
   cannot be shown, as any is.
2. **About never waits for a cover.** It answers with the covers it has
   kept. Those it has not are marked as being fetched and handed to a
   process of its own, which outlives the run and fetches them side by
   side; while any is on its way the answer says `"again_after": 3`. A
   cover is kept a week, so is knowing there is none; one that failed is
   left an hour.

## Verification

- `plugins`: `again_after` read, and refused out of place; About answers
  without the cover it has not kept, asking again, the cover fetched
  meanwhile and shown the next time.
- `bench-main-window`: a panel asking again is asked again about the same,
  and not once its answer stops asking.

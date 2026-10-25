# ADR-0329: A lookup is asked again before it fails

## Status

Accepted, 2026-10-25. Extends ADR-0261 (asked to slow down, MusicBrainz is
asked again).

## Context

Identify sometimes failed at once: MusicBrainz did not answer -- a dropped
connection, a timeout, a 502 from its proxy -- and the engine handed the
failure straight on, though asking again a moment later would have worked.
Only a 429 or 503, MusicBrainz asking to slow down, was asked again.

## Decision

- Every fetch an engine makes for a client -- MusicBrainz, the Cover Art
  Archive, Bandcamp -- that gets **no answer** (refused, dropped, timed out)
  or **a failure on the service's side** (500, 502, 504) is asked again
  after a second, and once more after three, before the failure is handed
  on as before. Cancelling stops the waiting.
- An answer is taken as it is: a 404, a 400, a 503 (ADR-0261's own).
- A connection the service does not take is given up on after ten seconds,
  not the thirty a whole request may take, so the next try comes sooner.

## Verification

- `engine-file-work`: no answer and 500/502/504 are asked again after one
  and three seconds, then given up on; 200, 400, 404 and 503 are not.

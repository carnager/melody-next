# ADR-0154: Explicit mirror root for conversions

Date: 2026-09-12

Status: accepted

## Context

Mirror mode (ADR-0132) reproduces the folder structure below the
sources' deepest common directory. That inference is unstable in
exactly the way a live report demonstrated: converting several albums
infers a high common root and lands `Artist/Album/…` under the
destination, while converting one album infers the album directory
itself — every relative path collapses to a bare filename and the
files dump flat into the destination root, despite a preview that
looked right in earlier runs. The mirrored layout of a conversion must
not depend on how broad the selection happens to be. ADR-0132 recorded
the editable mirror root as the follow-up; this is it.

## Decision

The convert dialog gains a persisted "Mirror below" path beside the
mirror checkbox. When set, it is the mirror root: each source's
relative directory is its byte-exact location below that root, and the
existing planner check reports sources outside the root as blocking
problems (in the bounded problems pane). When left empty, the previous
behavior remains — the placeholder says so and the preview names the
inferred root, as it always has.

The field persists like the dialog's other choices, so a library
rooted at one place (`…/Rips/flac`) mirrors identically whether one
album or fifty are selected.

## Consequences

- Single-album conversions finally create their `Artist/Album`
  folders when the root sits above the artist directories — the
  reported bug.
- An explicit root that does not contain a selected source blocks the
  plan visibly instead of guessing; the empty-field default keeps old
  workflows byte-identical.

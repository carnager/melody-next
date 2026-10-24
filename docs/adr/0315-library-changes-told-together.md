# ADR-0315: Library changes told together

## Status

Accepted, 2026-10-25. Changes how `catalogue.changed` is sent (ADR-0222's
events) and how the library panel follows it.

## Context

An album retagged in the tag editor took seconds -- on a slower machine,
reportedly minutes -- to show in the library panel, though the engine had
the new tags at once (`melody-cli find` showed them). The engine re-reads
each written file into its library inside that file's commit (ADR-0126)
and told every client of each one: sixteen files, sixteen
`catalogue.changed` events, spread over the write. The window answered each
by restarting a 250 ms wait and then re-querying every level of the library
tree it shows -- with a grouped view (ADR-0254), each after the engine had
rebuilt the view from the whole library, the library having changed since
(1.6 s on a library of tens of thousands of tracks) -- and the refreshes
queued one behind another.

## Decision

1. **The engine tells a batch.** Changes are gathered and told as one
   `catalogue.changed` once none has come for 300 ms, and at most a second
   after the first, so a long write is still seen to go on: each path and
   album once, `everything` if any said so. Files are still re-read in their
   commits; only the telling waits.
2. **One refresh runs, at most one waits.** A refresh of the library panel
   after a change drops the queries an earlier one still has waiting, which
   it asks again anyway.
3. **`--debug` says it**: the changes an engine tells, and how long the
   panel's refresh took.

Refreshing only the levels a change touches, and patching a grouped view's
cache rather than rebuilding it, remain open.

## Verification

- `engine-methods`: three changes told as one, each path and album once;
  nothing more without a change; a long run told as it goes; everything
  said.

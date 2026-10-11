# ADR-0278: A file job's result names what was written, not its tags

## Status

Accepted, 2026-10-11. Amends ADR-0237 (engines do the file work); builds on
ADR-0268 (what shows a library follows its changes).

## Context

Tagging thousands of files failed with "the engine closed the connection".
Trackknife hands an Apply to the engine in parts of about half a megabyte
of plan, but the answer to each part carried every written file's complete
tags, read back after the write -- for files with large tags (a waveform,
lyrics, an AcoustID fingerprint) many times the plan. A part's answer of
2 MB arrived; one of 5 MB passed the 4 MiB a connection holds for events
not yet read, and the engine let the client go. The files of that part were
written; the window never learned it.

The tags were sent so the window could show them in its lists. Since
ADR-0268 the lists follow their engine's changes, and a write is one.

## Decision

- **A result says what was written:** per file its state, its journal and
  backup, its identity before and after, and any problem -- never its
  tags. Tag writes, cover writes, renames and moves, and undo answer so.
  A result grows by a few hundred bytes a file, whatever the tags.
- **The window reads the tags written from the files**, as it reads them for
  the tag editor: the rows at the paths written, in batches of 256 from the
  engine each list is of, a moment after the write. The library's own
  change notice describes the rows of files it indexes as well.
- **Protocol level 10.** An older client expecting the tags gets none, and
  its rows keep what they showed until read again.

## Consequences

- An Apply of any size answers within the connection's bounds; the parts
  stay the bound on what one request carries.
- A row shows its new tags a moment after the write, not with it.
- Tested: twenty files with 50 KB tags each answer in under 2 KB a file, and
  fail that test with the tags put back.

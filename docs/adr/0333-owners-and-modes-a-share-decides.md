# ADR-0333: Owners and modes a share decides

## Status

Accepted, 2026-10-26. Completes ADR-0314 (writes go on where a share
decides file modes) and ADR-0111 (ownership on limited destinations).

## Context

A file is rewritten or moved through a prepared copy, given the original's
owner, mode and extended attributes before it takes its place. ADR-0314
let the mode go where the share refuses to set it -- but the check that
follows still compared the copy's mode, and owner, with the original's.
So on a share deciding modes itself, every file whose mode differs from
the one the share gives new files was refused: "Prepared-copy ownership
or permissions differ from the source" for whole albums, while albums
whose files happened to have the share's mode went through -- "mostly it
works". A share may also take a `chmod` or `chown` without error and not
keep it.

## Decision

- **What the copy holds once its owner and mode were set is what the
  destination keeps.** Where that is not the original's -- refused, or
  taken and not kept -- the write goes on, and a publication says the
  ownership or permissions were not preserved: the destination decides
  them.
- **The check that follows asks that it did not change from there**
  while the extended attributes were set, not that it is the original's.
- On a filesystem that keeps them, nothing changes: the copy holds the
  original's owner and mode, as checked before.

## Consequences

- On such a share a rewritten or moved file has the owner and mode the
  share gives its files, as every file made there has.
- Recovery after an interrupted publication still compares a candidate's
  mode with the original's; on such a share a candidate may then be set
  aside for attention rather than taken -- nothing is lost.

## Verification

- `core`: a copy with the original's owner and mode keeps them; another
  mode, another group, said; a copy changing after it settled told.

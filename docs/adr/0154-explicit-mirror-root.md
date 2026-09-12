# ADR-0154: Mirror conversions recreate the full source path

Date: 2026-09-12

Status: accepted

## Context

Mirror mode (ADR-0132) reproduced the folder structure below the
sources' deepest common directory. That inference is unstable in
exactly the way a live report demonstrated: converting several albums
infers a high common root and lands `Artist/Album/…` under the
destination, while converting a single album infers the album
directory itself — every relative path collapses to a bare filename
and the files dump flat into the destination, despite a preview that
looked right on earlier, broader runs. A first fix attempt added an
explicit "mirror root" path field; live feedback rejected it as a
confusing second destination knob next to the existing "Into" folder.

## Decision

Mirror mode recreates each source's complete directory path beneath
the destination: `destination + full source path`. No inference, no
extra option — the planner receives the filesystem root as the mirror
root (its relative-path derivation already handled `/`; only the
validation learned to accept it). The preview announces "Recreating
full source paths beneath the destination" and shows the exact
relative paths.

The result is deterministic and independent of selection breadth: one
album or fifty, the converted files always land under the same
destination-rooted copy of their source tree.

## Consequences

- The reported bug is gone structurally — there is no root to guess
  wrong.
- Destination trees carry the full source hierarchy (for example
  `Music/mnt/nas/Music/Rips/flac/Artist/Album/…`). That depth is the
  explicit trade-off of the requested rule; trimming it is what the
  layout expressions are for when mirror mode is off.
- `common_source_directory_raw_path` remains for other callers; the
  convert dialog no longer uses it.

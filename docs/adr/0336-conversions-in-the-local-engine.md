# ADR-0336: Conversions run in the engine on the asker's computer

## Status

Proposal, 2026-10-26.

## Context

Conversions run in two places: Melody's Convert window converts in its own
process -- files of another engine fetched first -- and an engine's
`convert.apply` converts its own files for `melody-cli`. Two code paths for
one thing, the second without the window's options or Cover settings.
Converting on the engine that holds the files would be one path, but a NAS
engine's CPU is far weaker than a desktop's.

## Decision (proposed)

- **Every Melody has an engine on its own computer** -- Linux already; the
  macOS app starts one too, perhaps built into its binary. Melody
  decision, 2026-10-26.
- **A conversion is a job of that engine**: it fetches the sources from
  whichever engine holds them, converts on this computer's CPU, and writes
  where the window says -- the window's options and Cover settings in the
  request. Converting on the engine with the files stays possible, chosen
  where that is better.
- The Convert window's own conversion code goes.

## Unknown

- How the macOS app starts or embeds its engine.
- Writing a conversion's output onto another engine ("archive to another
  engine", 2026-10-16).

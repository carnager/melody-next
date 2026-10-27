# ADR-0336: Conversions run in the engine on the asker's computer

## Status

Accepted, 2026-10-26. Superseded on the branch `mpd-client` by ADR-0340:
Melody converts itself, and there is no local engine.

## Context

Conversions ran in two places: Melody's Convert window converted in its own
process -- files of another engine fetched first -- and an engine's
`convert.apply` converted its own files for `melody-cli`. Two code paths for
one thing, the second without the window's options or Cover settings.
Converting on the engine that holds the files would be one path, but a NAS
engine's CPU is far weaker than a desktop's.

## Decision

- **Every Melody has an engine on its own computer** (ADR-0226): systemd's
  on Linux, one Melody starts on macOS -- perhaps built into its binary
  some day. Melody decision, 2026-10-26.
- **A conversion is a job of that engine**, `convert.files` (protocol level
  26): each item's source, destination and tags, and the window's options
  -- the preset itself, sample rate, bit depth, channels, gain, cover and
  its limit (ADR-0335) -- converted on a pool as large as asked and the
  machine's cores allow, the folders made by the engine, every item
  answered in order. In jobs under the engine's line limit.
- **Files of another engine** are fetched as before into a folder of this
  computer's, and converted from there: this computer's CPU, whatever holds
  the files.
- **The Convert window converts nothing itself.** Without this computer's
  engine it says so.
- `convert.apply` stays the engine's own conversion of its files, for
  `melody-cli`; both run the same converter.

## Unknown

- Converting on the engine with the files, chosen where that is better.
- Writing a conversion's output onto another engine ("archive to another
  engine", 2026-10-16).

## Verification

- `bench-main-window`: the Convert window's conversions -- a selection, a
  file fetched from another engine, a permanent gain -- carried out by the
  test's engine.

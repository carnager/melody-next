# ADR-0340: Melody converts

## Status

Accepted on the branch `mpd-client`, 2026-10-27. Supersedes ADR-0336 there.

## Context

ADR-0336 had the Convert window hand its conversions to the engine on this
computer, so that one path converted. On the branch `mpd-client` there is no
local engine: one server owns the library and playback, and Melody does the
file work on this computer -- with its CPU, which is the point of converting
here rather than on a home server or a Raspberry Pi.

## Decision

- **Melody converts**, in its own process, with the shared converter: the
  window's options, Cover settings' limit for lossy covers (ADR-0335), the
  folders made first. Files of an engine elsewhere are fetched to this
  computer first, as before (ADR-0237 stage 6).
- The `convert.files` job and its encodings are gone; nothing asks an
  engine to convert for the window.
- `convert.apply` stays for now, an engine converting its own files for
  `melody-cli`; in this model melody-cli would convert on its own computer
  too (a later slice).

## Verification

- `bench-main-window`: the Convert window's conversions -- a selection, a
  file fetched from another engine, a permanent gain -- carried out by
  Melody.

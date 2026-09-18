# ADR-0171: Close M5 and bound format expansion

## Status

Accepted.

## Context

M5's exit gate is a safe, fast local tagging and file-operation workspace. Its
criteria require preservation proof for every *advertised* writable format;
they do not require mutation support for every decodable container. Remaining
notes mixed gate requirements with unbounded future adapter expansion.

## Decision

M5 is complete after ADRs 0169 and 0170. Its qualified initial matrix is text
mutation for FLAC, WavPack, MP3/ID3v2, single-stream Vorbis and Opus, and
MP4/M4A; embedded artwork mutation for FLAC, MP3 APIC, and MP4 `covr`; and
versioned `linux-v1` and `portable-v1` path policies.

Unsupported writers remain visibly read-only. Musepack, Monkey's Audio, Ogg
artwork, and additional containers require independent real-file preservation
qualification before their capability flag changes. They are format expansion,
not unfinished M5 infrastructure.

User-authored sanitizers are not accepted: arbitrary replacement programs are
difficult to make portable, deterministic, and safely migratable. New policies
must be named versioned built-ins. Neither v1 policy normalizes Unicode; a
future normalization policy requires evidence and a new identity.

Exact values and `tkfmt-1` conditions remain the qualified transformation match
surface. Regex/glob/date matching and automatic `TOTALTRACKS` derivation may be
new typed actions, but cannot alter existing actions. General metadata/artwork
sidecars, companion-file moves, empty-folder cleanup, and broader undo are
separate follow-ups.

## Exit-gate evidence

- Bulk/keyboard editing, ordered values, dynamic fields, transformations, and
  saved field layouts have core and offscreen UI coverage.
- Every enabled writer has a real fixture round trip checking intended values,
  unrelated native data/artwork, MusicBrainz fields, and audio/container
  preservation appropriate to that adapter.
- Write and publication tests cover revision conflicts, injected failures,
  rollback, durable recovery evidence, and conservative reconciliation.
- Repository and UI tests cover revision-qualified all-occurrence relocation,
  duplicate/logical rows, cached metadata, and active playback bindings.
- The development suite, formatting, and SPDX gates are the closure verifier;
  release-scale stress and packaging remain M10.

## Consequences

M7 becomes the active acceptance gate. Format dimensions remain independent;
decode support never implies writable support. Later adapters extend the
product without reopening M5.

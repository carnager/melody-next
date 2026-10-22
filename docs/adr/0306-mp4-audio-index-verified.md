# ADR-0306: An MP4 write proves its audio index still points at the audio

## Status

Accepted, 2026-10-22. Completes ADR-0136's qualification proof for MP4.

## Context

- **The proof covered the audio, not the way to it.** A tag write is made
  on a copy and refused unless `ftyp`, every `mdat` and every other box but
  `moov` and padding are byte-identical (ADR-0136). But `moov`, which TagLib
  rewrites, also holds each track's sample table: where every chunk of
  audio is in the file. In a "fast start" file `moov` is before the audio,
  so a tag that grows it moves the audio, and TagLib shifts every chunk
  offset by as much. That shift was taken on trust: a wrong one leaves the
  audio intact but unreachable -- a file that no longer decodes -- and the
  proof passed it.
- It came up when a library failed to convert: its damaged files were not
  Melody's doing (MP3s among them, whose audio frames Melody's writer
  already proves identical), but for MP4 the claim could not be proven.

## Decision

1. **Every chunk offset is accounted for.** For each track, in order, the
   prepared file's `stco` or `co64` entries must be the source's, each moved
   by exactly as much as the box it points into moved -- the same `mdat`,
   found by the box comparison. An offset into no preserved box, a table of
   another length or kind, or a track gone, refuses the write.
2. **The samples' own tables are unchanged:** `stsd`, `stts`, `ctts`,
   `stsc`, `stsz`, `stz2` and `stss` byte-identical.
3. It is part of the shared proof, so tag and artwork writes alike are held
   to it.

## Verification

- `metadata-mp4-writer`: the fixture made fast start, its decoded audio
  identical; a 20 KB comment written by TagLib moves the audio and passes;
  the same file with its offsets left unshifted, and with one chunk off by a
  byte, is refused -- and, with the check taken out, both were accepted.

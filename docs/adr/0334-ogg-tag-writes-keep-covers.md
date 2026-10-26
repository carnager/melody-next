# ADR-0334: Ogg tag writes keep the covers

## Status

Accepted, 2026-10-26. Extends ADR-0114 (qualified Ogg comment writes).

## Context

Opus and Vorbis files carry covers in their comment packet: as
`METADATA_BLOCK_PICTURE` entries (a base64 FLAC picture block) or, in older
files, `COVERART` with `COVERARTMIME`. Melody writes tags there through
TagLib, which reads the pictures it understands into a list of its own and
writes that list back. A picture it cannot read -- a block it rejects as
malformed -- is dropped from that list and so from the file: a ReplayGain
write took the cover with it, unannounced. The packet check (ADR-0114)
proved the audio untouched but not the comment's covers.

## Decision

- **A tag write keeps the file's covers as they were.** Once the comment is
  written, the cover entries of the source and the prepared copy are read
  from their comment packets and compared -- base64 by the bytes it
  carries, so a picture rewritten unchanged counts as kept, in any order.
- **Where they differ, the source's entries are put back** as they were
  stored, every one of them, and compared again; a copy whose covers still
  differ is refused -- "the file's cover would be lost by this tag write"
  -- rather than published.
- Writing covers into Ogg files on purpose is not part of this; ADR-0337
  decides it.

## Consequences

- One more read of both comment packets per Ogg tag write; a second save
  only where covers were lost.

## Verification

- `metadata-ogg-writer`: a Vorbis and an Opus file carrying a picture
  TagLib reads, one it cannot, and a `COVERART` with its mime type keep all
  four through a ReplayGain write -- without the guard, the unreadable one
  was lost.

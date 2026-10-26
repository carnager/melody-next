# ADR-0337: Covers in Opus and Vorbis files

## Status

Accepted, 2026-10-26. Extends ADR-0137 (qualified MP3/MP4 artwork) to Ogg;
follows ADR-0334, which kept Ogg covers read-only until this.

## Context

Opus and Vorbis files carry covers in their comment, as FLAC picture blocks
(`METADATA_BLOCK_PICTURE`). Melody's conversions write them there
(ADR-0131) and its players show them, but the tagger did not read them: the
Artwork tab showed no cover, and Add image, Fetch cover and Identify's
covers could not write one.

## Decision

- **Read** like FLAC's pictures -- role, type, description, size -- for the
  Artwork tab, the compact cover and every use of an embedded cover (copy,
  export, conversion).
- **Written** -- added, replaced, removed -- through the same plans,
  journal and undo as every cover, with TagLib: the comment's pictures
  rebuilt in order with the change in its place, a replacement keeping the
  original's type and description, an added one taking its role's. Proven
  as every Ogg write is (ADR-0114): every packet but the comment
  byte-identical; and the covers read back exactly as planned.
- **Only where TagLib reads every cover the comment holds.** An older
  `COVERART`, or a picture block it cannot parse, would be lost or changed
  by a write through it; such a file's covers are shown and not written --
  its own adapter name, not a qualified one, says so.
- The container formats name their tag-writing step and their pairing of
  cover and tag adapters once (`text_adapter_for_artwork`), where three
  places spelled them before.

## Verification

- `metadata-ogg-writer`: Opus and Vorbis covers read, replaced in place,
  removed, added with role and description, two changes at once -- the
  other covers and the audio as they were; a `COVERART` beside them keeps
  the file read-only.
- `metadata-commit`: a cover added to an Opus and a Vorbis file committed
  through the journal and undone byte for byte.

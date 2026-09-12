# ADR-0155: Conversions write tags through the tag layer

Date: 2026-09-12

Status: accepted

## Context

Conversions transferred text tags by feeding FFmpeg's metadata
dictionary and letting each muxer serialize it. That routes every
field through the muxers' key-conversion tables, which rename keys
case-insensitively behind our back — the generic `comment` key became
`DESCRIPTION` in every vorbis-family output, failing the reread
verification for whole albums, and was patched with a targeted rename
(ADR-0154's sibling fix). Whatever key the tables touch next fails the
same way, and ID3 output demoted everything unknown to TXXX frames by
way of a hand-kept name map. The live request is simple: converted
files must keep all source tags.

The project already owns proven tag writing: TagLib's property layer
is what the qualified writers and the loudness pipeline build on, and
it maps names natively per format (USLT for lyrics on ID3, proper
Vorbis field names, TXXX only where nothing better exists).

## Decision

FFmpeg writes audio; tags are written afterwards by the tag layer.

- The encoder pipeline no longer feeds text metadata into the muxer
  dictionary at all (the container carries only what the muxer itself
  emits, such as its encoder note). Artwork keeps its existing
  qualified path — the attached-picture stream or
  `METADATA_BLOCK_PICTURE` — unchanged.
- After encoding completes, the finished temporary reopens through
  TagLib and receives the full transfer document as one property map:
  every effective field under its exact native name, all values,
  multi-values as lists. Pictures live outside the property view, so
  the write never disturbs the embedded cover. The loudness strip
  (ADR-0133) still applies to the transfer document before this.
- The targeted DESCRIPTION→COMMENT rename becomes obsolete and is
  removed; the reread verification stays exactly as strict as before
  and now runs against tags produced by the same layer that reads
  them.

What still does not transfer, unchanged and documented: foreign
native binary objects the target format cannot represent, and
additional pictures beyond the one resolved cover (ADR-0131).

## Consequences

- "All tags survive" becomes structural: no per-key conversion tables,
  no name allowlists, and one write path shared with everything else
  that mutates tags.
- ID3 targets gain real frames (USLT lyrics, proper multi-value
  handling) instead of the previous TXXX fallback for anything beyond
  the hand-kept map.
- A field name invalid for the target format simply fails the honest
  verification instead of being silently dropped — same failure
  surface as before, better provenance.

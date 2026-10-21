# ADR-0305: Empty tag values are not carried into a converted file

## Status

Accepted, 2026-10-22. Refines ADR-0133's transfer of tags on conversion.
Found, with the test case, by ikloog.

## Context

- **A file with an empty tag could not be converted at all.** A real MP4
  file carried `ACOUSTID_FINGERPRINT` and `ACOUSTID_ID`, each with one empty
  string as its value. The converter writes the source's tags to the new
  file and rereads them, refusing the file unless every tag comes back
  exactly as written. No preset's file keeps an empty value -- WAV, FLAC and
  Opus alike reread without it -- so every conversion of such a file failed,
  though nothing of its tags was lost.
- **Excusing the absence in the check was the first proposal.** It would
  have let the reread differ from what was meant, and left a tag of a real
  value and an empty one still failing.

## Decision

1. **A converted file is not given empty values.** Where the converter
   decides what the new file carries -- the source's tags but its stale
   loudness claims (ADR-0133) -- each tag's empty values are left out, and a
   tag left with none is not written.
2. **The reread stays exact.** It is compared with what was meant to be
   written, as before; nothing is excused.
3. **Only the new file.** The source is never written; the tag writers and
   their round trip are unchanged.

## Verification

- `audio-convert`: every preset converts a file whose AcoustID tags are
  empty and one of whose tags has a real value and an empty one: the empty
  tags are absent from what was made, the real value kept. Without the
  decision, every preset refused it.

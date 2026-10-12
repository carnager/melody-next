# ADR-0279: Empty values are dropped when a file is written

## Status

Accepted, 2026-10-12. A Trackknife decision; refines the rule that unknown
metadata is preserved when a file is rewritten.

## Context

A tag whose value is empty -- an ID3 frame, a Vorbis comment `ASIN=`, an
MP4 atom with nothing in it -- is left behind by many taggers. The writers
treated it as metadata to preserve. TagLib cannot write an empty Vorbis
comment or APE item back, so the planner refused every FLAC and WavPack file
holding one, and -- more strictly than needed -- every MP3: a whole album of
Hebrew MP3s could not be tagged at all, for fields nobody could see.

An empty value holds nothing. What is lost by dropping it is that a name
once had no value.

## Decision

- **A file being written drops its empty values.** A field with no value is
  removed; the empty values beside others are taken out of it. Only files
  written anyway: opening, reading or playing a file changes nothing.
- **Each drop is a change of the plan**, addressed to that exact spelling,
  so the reread after the write checks it like any other change. Everything
  with a value is preserved as before.
- **Shown where edits are shown.** The tag editor's grid shows an empty value
  as "(empty · dropped when written)".
- The refusal of untouched empty fields is gone, for every format.

## Consequences

- Files with stray empty fields take writes in every format.
- A field another program created empty, on purpose, is gone after a write.
  No known program depends on that.
- TagLib does not even read an empty Vorbis comment: in FLAC it was never
  seen, and a write always dropped it. The rule now says so.
- Tested: the planner drops the empties of FLAC, MP3 and MP4 sources, each to
  its exact spelling, and keeps a field's other values; a real MP3 with an
  empty TXXX and an empty TPE2, and a real FLAC with an empty comment, take a
  title write and lose only the empties.

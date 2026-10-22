# ADR-0307: A converted file's tags are written as Picard writes them

## Status

Accepted, 2026-10-22. Refines the transfer of tags on conversion
(ADR-0133, ADR-0305). **Melody decision:** MusicBrainz Picard's conventions
are the reference for how a tag is written into a format.

## Context

A library failed to convert on two kinds of tag, each of whose names the
target -- Vorbis comments, in FLAC, Ogg and Opus -- cannot hold: a name
there is plain ASCII from space to `}`.

- **`~length`**, written into real M4A files by some tool: Picard's own
  variable, the track's length, never meant for a file. `~` is outside
  what a Vorbis comment may be named.
- **`performer:agogô`**, a musician credited with an instrument, as Picard
  names it and as an MP3 holds it (`TMCL`); `ô` cannot be in a Vorbis
  comment's name.

TagLib left both out, the reread lacked them, and the converter refused
the file, as it must when what was written is not what was meant.

## Decision

The converter decides what a new file carries as Picard would write it,
and the reread is held exactly to that:

1. **No name starting with `~`**, in any format: Picard keeps those to
   itself and never writes one.
2. **A performer of an instrument, in Vorbis comments,** is
   `PERFORMER=Name (instrument)` -- Picard's form there -- after the
   performers credited plainly. MP3, WAV (ID3) and M4A keep
   `performer:instrument` as they hold it.

## Verification

- `audio-convert`: every preset converts a source with `~length` and with
  performers plain and of an instrument: `~length` absent everywhere;
  FLAC, Ogg and Opus with `PERFORMER` = "Lead Singer", "Bell Player
  (agogô)"; MP3 and WAV with `performer:agogô` kept. Before, FLAC, Ogg and
  Opus refused it, as the library had.

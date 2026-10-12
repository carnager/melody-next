# ADR-0281: Tags are written in place where they fit

## Status

Accepted, 2026-10-12. Amends ADR-0237 (engines do the file work), ADR-0248
and ADR-0280 (backups); keeps ADR-0263 (undo restores what was written).

## Context

Every tag write copies the whole file, writes the tags into the copy,
compares the copy's audio with the original's, keeps the original as the
backup and renames the copy into place. That is safe, and it moves the file
three times or more to change a few kilobytes. Over a network share a
library of 18,000 ALAC files took most of a day.

Most files have room for their tags: FLAC padding, ID3v2 padding, an MP4
`free` atom. TagLib's own save then rewrites only the header: on a 143 MB
ALAC file a new title changed the bytes from 40 KB to 1.2 MB and nothing
after, the file's length the same.

## Decision

- **Written in place where it fits.** The tags are written by TagLib as
  before, but into an overlay over the file, read-only: reads come from the
  file, writes are kept. If the save would change the file's length -- the
  tags do not fit -- nothing is written and the file takes the copy path as
  before. Otherwise the overlay is the whole change: a few ranges of bytes,
  all in front of or around the audio, never in it.
- **Checked before the file is touched.** The tags are read back from the
  overlay and compared with the plan, as a written copy is; the ranges are
  checked to lie outside the audio. Only then is anything written.
- **The backup is the bytes replaced.** Before writing, the ranges' old and
  new bytes are written to the backup file, synced, and journaled. Undo
  writes the old bytes back -- the file is the original byte for byte, as
  ADR-0263 says -- after checking the ranges still hold what was written.
- **Then the ranges are written and synced**, and the tags read back from
  the file must be those read from the overlay.
- **A crash puts the original back.** A write interrupted after its backup
  was kept writes the old bytes back; one interrupted before has touched
  nothing.
- The file keeps its inode. Its modification time changes, as with any
  write. Formats TagLib rewrites whole (Ogg, WavPack) take the copy path.

## Consequences

- A tag edit that fits moves the header and its backup, not the file: on
  the ALAC file above about 2.5 MB instead of about 1 GB.
- A player reading the file while its header is rewritten may see a moment
  of mixed header; the audio is never touched.
- Schema 54: the journal says which writes were in place.
- Ratings go the same way: MP3's FMPS frame and Windows Media Player's POPM,
  MP4's FMPS atom are written on the overlay as on a copy.
- TagLib caps padding at 1% of a file (at least 4 KB for FLAC, 1 KB for
  ID3v2): a much shorter tag in a tiny file is resized, and takes the copy.
- Tested: on FLAC, MP3 (with a rating) and MP4 (with a colon-named atom) the
  ranges laid on the file are byte for byte the prepared copy; tags past
  their room take the copy; a commit in place keeps the file's inode and
  length, rolls back and undoes byte for byte; and a write torn half way is
  rolled back by recovery.

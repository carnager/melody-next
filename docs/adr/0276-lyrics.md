# ADR-0276: Lyrics, read by the engine and shown beside playback

## Status

Accepted, 2026-10-10. Builds on ADR-0220 (the engine answers) and ADR-0237
(engines do the file work).

## Context

Trackknife shows no lyrics. Many libraries already carry them: embedded as
a `LYRICS` tag (Vorbis comments, ID3 `USLT`, MP4 `©lyr` -- TagLib maps all
three to the one key), as `UNSYNCEDLYRICS`, as ID3 `SYLT` frames with
timings, or as an `.lrc` file beside the audio. Often the embedded text is
itself LRC, with `[mm:ss.xx]` timings.

The files belong to the engine, which may be on another machine, so the
client cannot open them to look.

## Decision

- **The engine finds them.** A new method, `catalogue.lyrics {path}`, answers
  with the lyrics of one file, or `null` if it has none. It looks, in this
  order, and takes the first *synced* lyrics it finds, or else the first
  plain ones:
  1. an `.lrc` beside the file, with the same name (`Song.flac` → `Song.lrc`,
     compared case-insensitively);
  2. ID3 `SYLT`;
  3. the `LYRICS` tag, then `UNSYNCEDLYRICS`.
  Synced wins over plain whatever its source, because a file often carries
  plain text embedded and an `.lrc` beside it that someone fetched later.
- **The answer** is `{source, synced, lines: [{time_ms?, text}]}`, the
  offset already applied.
  `source` says where they came from (`sidecar`, `sylt`, `tag:LYRICS`, ...)
  so the panel can say so. LRC is parsed in the engine by one pure function:
  several timings on a line give it several entries, `[offset:]` is applied,
  word timings (`<mm:ss.xx>`) are dropped -- lines only -- and other header
  tags (`[ar:]`, `[ti:]`) are ignored. Text that has no timings is plain. An
  `.lrc` that is not UTF-8 is read as Latin-1. Like a cover, lyrics are read
  only for a track in the library or one the engine holds to play.
- **Protocol level 9.** An engine below it has no lyrics; the panel says so
  rather than failing.
- **A Lyrics panel** in Trackknife, a dock like Up Next, shows the playing
  track's lyrics and follows it as it changes. Synced lyrics highlight the
  current line and keep it centred; the position is carried on locally
  between the engine's reports, so the highlight moves smoothly. Clicking a
  line seeks to it. Plain lyrics are a scrolling page of text. The panel
  shows where the lyrics came from and says "No lyrics" plainly when there
  are none. Hidden in the shipped layout; Playback › Lyrics (Ctrl+Shift+L)
  shows it.
- **Asked for as each track starts** while the panel is open, and when it
  is opened; hidden, it asks nothing. One small request a track needs no
  cache, and an edited file shows its new lyrics the next time it plays.

## Not in this decision

- **Fetching lyrics from the internet** (LRCLIB and others). That is the
  intended first use of a plugin boundary, decided separately; what it
  fetches would be offered for review and saved as an `.lrc` or a tag
  through the usual preview.
- **Editing lyrics** beyond what the tagger already does with the `LYRICS`
  field, and writing `.lrc` files.
- The phone app; it can use the same method later.

- **The library's field index keeps `LYRICS`,** as it keeps every tag, so
  embedded lyrics stay searchable (`lyrics HAS ...`).

## Consequences

- Lyrics work the same for a local and a remote engine, and need no tag
  writes.
- `SYLT` needs reading the ID3v2 frames directly, as POPM already is.
- Tests: the LRC parser against a corpus (multiple timings, offsets, word
  timings, malformed lines, BOMs, CRLF); real files of each container with
  embedded lyrics; an `.lrc` beside a file; a remote engine below level 9.

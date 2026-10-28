# ADR-0344: The queue, Up Next and a list, shown by melody-cli

## Status

Accepted, 2026-10-28.

## Context

melody-cli could add to the queue and Up Next and play a list, but not show
any of them: `status` only counted them, `lists` only named the lists, and
nothing said which list the queue was playing from. The engine knew that
list (ADR-0253) but never said it. A line per entry formatted like
`current` needs the library's whole row for each file, which only the
engine has, the same reason `current` and `find` are formatted there.

## Decision

- **The engine formats entries on request.** `playback.queue`,
  `playback.requests` and `list.get` take `format` (tkfmt-1); each entry
  then also carries `text`. It is formatted with the library's row for its
  file, as `catalogue.find` is, or, for a file the library does not index,
  with what the entry or list item says of itself (title, artist, album,
  date, length). The rows are asked at once; when a file is not indexed,
  each is asked alone. One function does it, `Catalogue::format_entries`
  (with `format_known` where an engine has no catalogue). Protocol level 28.
- **`playback.queue` answers `list`**, the list the queue plays from, or
  null.
- **melody-cli:**
  - `queue [--format EXPR]` prints the queue, one entry a line, and says on
    stderr which list it plays from. When what plays is in the queue it is
    marked `> ` and the rest indented to match; when it is not (a track
    from Up Next, say), no line is marked or indented.
  - `upnext` alone prints Up Next in the order it plays; with words it
    adds, as before.
  - `lists NAME...` prints one list's tracks, found as `play list` finds
    it.
  - All three take `--format` as `current` does (same default) and print
    the engine's answer with `--json`. From an engine older than level 28
    they fall back to "Artist — Title" from the entries.
- Completion offers list names after `lists`, and `--format` after
  `queue`.

## Consequences

- A list named "folder" is reached as `lists folder` only for the playlist
  folder; to see its tracks, give more of its name.
- Tested in `melody-cli`: the queue with what plays marked, Up Next in
  order formatted from the library, the queue's list, a list's tracks, and
  a list item outside the library formatted from the list.

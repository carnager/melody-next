# ADR-0308: Hidden files and folders are not the library's

## Status

Accepted, 2026-10-22.

## Context

A library's conversion listed hidden files as its tracks: ChronoSync's
fragments of interrupted copies (`.CS080.05 Deja Vu.m4a`), 1 to 2 MB of a
track each, beside the real ones. The scan indexed every file with an audio
extension, hidden or not, and walked into every folder -- a trash, a sync
tool's state, macOS's `._` files beside tracks on a shared volume. None of
them is music anyone put in the library; each is a broken track in it.
MPD skips them too.

## Decision

1. **No hidden file is a track:** a name starting with a dot is never one,
   wherever the library is told of it -- the scan, a file the engine's
   watcher or `melody-watch` names (`core::is_audio_path`, the one rule for
   all of them).
2. **No hidden folder below a library folder is walked,** nor is a file in
   one taken when named to refresh. A library folder that is itself inside
   a hidden one -- `~/.local/share/music` -- is still the library's: only
   what is below it counts.
3. What was indexed of them before is gone from the library with the next
   scan, as any file no longer found is.

## Verification

- `local-library`: a library folder inside a hidden one, with a track, a
  ChronoSync fragment, a `._` file and a `.Trash` folder: only the track is
  indexed, and the fragment and a trashed file named to refresh are not;
  before, all four were tracks.

# Trackknife

Trackknife is a Linux music player and tag editor built with Qt 6. Connect to
MPD or Melody, or play local files through PipeWire. You can browse folders
directly or add them to a searchable library.

It takes ideas from foobar2000 and Cantata: album-grouped playlists, keyboard
controls, and tools for looking after a music collection. The name comes from
foobar2000's reputation as a Swiss Army knife for audio files.

![MPD queue with grouped albums and the server library](Screenshots/queue.png)

## Listening and browsing

Local playback is gapless. MPD and local files have separate queues; the active
tab determines which player the transport controls. Local lists survive a
restart, and MPD playlists can be opened and edited in their own tabs.

Both libraries browse artists, albums, and tracks, with covers and album counts.
Search results can be opened as a tab. For local files, searches use the library
database; opening results doesn't reread all your music. Press **Refresh** when
you want to scan for changes made outside the app.

Use **Copy to list → New tab…** or **Move to list → New tab…** to split out a
selection. You can also drop tracks from a local list onto empty tab-bar space
to create a tab. Dragging moves the entries; hold **Ctrl** to copy them. The
files stay where they are.

## Tagging and file tools

**Edit tags…** opens a tab where you can edit one file or a whole selection.
Changes stay in a draft until you apply them. MusicBrainz lookup works with
untagged albums too: search for a release, then arrange your files beside its
track list to check the assignments.

You can remove selected covers, fetch replacements, or add your own images.
Tags and embedded artwork are saved together. ReplayGain scanning is available
from the track menu, and the converter can copy a cover, resample audio, and
name its output from tags. Rename and move operations show the proposed paths
before changing files.

Text editing supports FLAC, WavPack, MP3, Vorbis, Opus, and MP4/M4A. Embedded
artwork editing supports FLAC, MP3, and MP4/M4A. Conversion presets cover FLAC,
MP3, Vorbis, and Opus, depending on the installed FFmpeg encoders. Playback
supports more formats than editing; see the [format table](docs/feature-matrix.md#format-support-dimensions)
for the limits.

## Searching your collection

Open **Workspace → Search…** (**Ctrl+Shift+F**) and choose **Library database**
to search the collection, or **Current tab** to search a local list. For a word search,
type an artist, album, or title. Enable **Query** for expressions such as:

```text
bitspersample EQUAL 24
REPLAYGAIN_ALBUM_GAIN MISSING
artist HAS "Nina Simone" AND date GREATER 1960
```

**Save as…** keeps a search so you can run it again. Result tabs contain the
matches from that run; they don't update automatically.

See the [library guide](docs/local-library.md#using-the-library) and
[query reference](docs/query-language.md) for more examples and details.

## Building

You need a C++23 compiler, CMake ≥ 3.28, Ninja, and pkg-config, plus these
libraries and their development headers:

- Qt ≥ 6.4, including Widgets, Concurrent, DBus, Network, and Test
- FFmpeg ≥ 6, TagLib ≥ 2.0, and libopenmpt ≥ 0.7
- libmpdclient ≥ 2.22 and libutf8proc ≥ 2.9
- PipeWire ≥ 0.3.50, libebur128 ≥ 1.2, and SQLite ≥ 3.37

Chromaprint's `fpcalc` is optional, for AcoustID fingerprinting.

```sh
cmake --preset release
cmake --build --preset release
./build/release/src/bench/trackknife
```

On Arch Linux, the package recipe builds from the repository's default branch:

```sh
cd packaging/arch
makepkg -si
```

There are no versioned releases yet. The [roadmap](docs/roadmap.md) lists the
unfinished work.

## Help and development

- [Melody setup](docs/melody.md): connecting and using remote speakers.
- [Local library](docs/local-library.md): adding folders, searching, and refreshing.
- [Formatting and scripts](docs/tkfmt.md): tag transformations and naming patterns.
  The app uses its own language, `tkfmt-1`; foobar2000 and Picard scripts aren't
  interchangeable with it.
- [Documentation index](docs/README.md): guides, specifications, and build checks.

## More screenshots

![Album and track search](Screenshots/search.png)

![Bulk tag editing](Screenshots/tagger.png)

![MusicBrainz lookup](Screenshots/musicbrainz.png)

![Conversion](Screenshots/converter.png)

![Renaming](Screenshots/renaming.png)

## License

GPL-3.0-only. See [LICENSE](LICENSE).

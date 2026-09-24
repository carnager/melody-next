# Trackknife

A music player and tag editor for Linux, in the spirit of foobar2000.

![A queue grouped by album, with the library beside it](Screenshots/queue.png)

## What's different

- **Playback runs in its own engine.** `melodyd` plays the music and
  Trackknife is one of its clients. Close the window and the music keeps
  going. Run the engine on a server and every client shares its library,
  queue and history.
- **Any machine can be a speaker.** Another computer, a Raspberry Pi, your
  phone. Switch mid-song and it carries on where it was.
- **A proper tag editor.** Edit many files at once, look albums up on
  MusicBrainz, fix covers, scan ReplayGain, convert and rename. You see every
  change before it's written.
- **Keyboard first.** Playlists grouped by album, with covers, and a shortcut
  for everything.
- **Search with a query language**, dynamic playlists from rules or Last.fm,
  and scrobbling done by the engine, so it works with the app closed.
- **An Android app** that controls the engine, plays on the phone and keeps
  albums for offline listening.

<p>
  <a href="Screenshots/tagger.png"><img src="Screenshots/tagger.png" height="180" alt="Editing the tags of a whole album"></a>
  <a href="Screenshots/musicbrainz.png"><img src="Screenshots/musicbrainz.png" height="180" alt="MusicBrainz lookup"></a>
  <a href="Screenshots/replaygain.png"><img src="Screenshots/replaygain.png" height="180" alt="ReplayGain scan"></a>
  <a href="Screenshots/converter.png"><img src="Screenshots/converter.png" height="180" alt="Converting to Opus"></a>
  <a href="Screenshots/renaming.png"><img src="Screenshots/renaming.png" height="180" alt="Renaming, with the new paths shown first"></a>
  <a href="Screenshots/settings.png"><img src="Screenshots/settings.png" height="180" alt="Settings"></a>
</p>

<p>
  <img src="Screenshots/android-newest.jpg" width="160" alt="The newest albums as covers">
  <img src="Screenshots/android-artists.jpg" width="160" alt="Artists, with letters down the side to jump through them">
  <img src="Screenshots/android-album.jpg" width="160" alt="An album page">
  <img src="Screenshots/android-queue.jpg" width="160" alt="The queue, grouped by album">
  <img src="Screenshots/android-nowplaying.jpg" width="160" alt="Now playing, coloured from the cover">
</p>

## Docs

- [Installation](docs/install.md)
- [Setup](docs/setup.md)
- [How the engine, agents and phone connect](docs/melody.md), including a
  headless `melodyd`
- [Formatting syntax](docs/formatting.md), for naming files and library views
- [Search queries](docs/query-language.md)
- [Everything else](docs/README.md)

This is new software and still changing. For a bug report, run
`trackknife --debug` in a terminal and include the output.

## License

GPL-3.0-only. See [LICENSE](LICENSE).

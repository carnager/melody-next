# Official plugins

Plugins that come with Melody: installed with it under
`/usr/share/melody/plugins/`, each **off** until enabled under
**Settings → Plugins**. One of the same name in
`~/.local/share/melody/plugins/` is used instead. They are written as any
plugin is ([docs/plugins.md](../docs/plugins.md)); an official one needs
nothing outside Melody -- no account, no service of anyone else's. Listen
together's server is melody-together, which is part of Melody.

| Plugin | What |
| --- | --- |
| [`web-search`](web-search) | Looks the album up on MusicBrainz, Discogs, Bandcamp, Rate Your Music or AllMusic, in the browser. |
| [`lrclib`](lrclib) | Lyrics from lrclib.net, synced where it has them, when the files have none -- kept a month. |
| [`about`](about) | A panel about the playing artist and album: the artist's photo, Wikipedia's summaries, where and since when, the album's date, kind and label, and links -- to Discogs, Bandcamp, Rate Your Music and AllMusic too -- from MusicBrainz, Wikidata and Wikipedia, kept a week. Either half can be turned off. Beneath them, the artist's albums missing from the library, from MusicBrainz -- or Discogs, with a token. |
| [`together`](together) | Listen along with friends (ADR-0302): host a room on your melody-together server, or join one with an invite, from Workspace › Run; a panel shows what plays, who listens and the invite. |
| [`scripts`](scripts) | Your own shell commands, run for each new album, when the library changed, every so many minutes, when Melody starts, or by hand from Workspace › Run. |

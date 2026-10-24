# Plugins

A plugin adds an entry to the context menu of track lists and of the library
-- "Share album", "Send to …" -- a side panel, lyrics, or a program run on the
library's news. A few official ones come with Melody
([`plugins/`](../plugins): the web search, a panel about the playing artist
and album, lyrics from LRCLIB, and Scripts, your own commands); everything
else is written and published apart. A plugin is a program of its
own, in any language; Melody runs it, tells it what was chosen in JSON, and
shows what it answers ([ADR-0288](adr/0288-plugin-actions.md)).

## Where

A folder per plugin, with a `plugin.json`:

```
~/.local/share/melody/plugins/albumshare/
    plugin.json
    share            (the program, executable)
```

On macOS your folder is `~/Library/Application Support/melody/plugins/`.
Plugins under `$XDG_DATA_DIRS/melody/plugins` are found too, and the official
ones installed beside Melody (`share/melody/plugins` next to its `bin`); one of
the same name in your own folder wins. `MELODY_PLUGIN_PATH`, a list of folders
like `PATH`, replaces all of these -- for working on a plugin elsewhere. A plugin found is **off** until you tick it
under **Settings → Plugins**. A plugin runs with your rights, like
any program you start: enable only those you trust.

## plugin.json

```json
{
  "schema": 1,
  "name": "Album share",
  "description": "A download link for an album, from your albumshare server",
  "actions": [
    {
      "id": "share",
      "label": "Share album",
      "for": "albums",
      "run": ["./share", "--quiet"],
      "result": "notify-and-copy",
      "timeout": 300
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `schema` | `1`. |
| `name`, `description` | What Settings shows. |
| `actions` | One or more menu entries. |
| `id` | The action's name, given back in the input. |
| `label` | The menu entry. |
| `for` | `albums` (the default): run once for each album chosen. `tracks`: run once with everything chosen. |
| `run` | The program and its arguments, without a shell. A first word starting with `./` is in the plugin's folder; any other is looked up in `PATH`. |
| `result` | What the output is for: `notify` (the default), `copy` (to the clipboard), `notify-and-copy`, `none`, or `list` -- rows in a window of their own (below). |
| `timeout` | Seconds before it is stopped, 1 to 3600 (default 120). |
| `settings` | Optional: what it asks to be set (below). |

An unknown key, or a value out of place, makes the plugin unusable, and
Settings says why.

### Settings

A plugin that needs an address or an account says so, and Melody asks for
it behind the plugin's **Settings…** button in **Settings → Plugins**, in a
dialog of its own ([ADR-0300](adr/0300-plugin-settings-and-optional-features.md)):

```json
"settings": [
  {"key": "url", "label": "Address", "default": "http://localhost:8080"},
  {"key": "token", "label": "API token", "kind": "secret",
   "help": "dropwatch: settings -> Account"},
  {"key": "covers", "label": "With covers", "kind": "toggle", "default": true},
  {"key": "site", "label": "Site", "kind": "choice",
   "choices": ["musicbrainz", "discogs"]}
]
```

| Key | Meaning |
| --- | --- |
| `key` | Its name in the input: `a`-`z`, `0`-`9`, `-` and `_`. |
| `label` | What Settings shows beside it. |
| `kind` | `text` (the default), `secret` (shown as dots), `toggle` (on or off), `choice` (one of `choices`). |
| `default` | Its value until one is set: text, `true`/`false` for a toggle, one of the choices. A secret has none; a choice's is its first. |
| `choices` | A choice's, as text. |
| `help` | Said beneath it, and when the pointer is over it. |
| `requires` | The `key` of a toggle listed before it: an optional feature it belongs to. It stands beneath that toggle, indented, greyed until the toggle is ticked. |
| `link` | A page about it, `http` or `https`, shown as a link beneath it -- for a feature, where to get what it works with. |

An optional feature is a toggle and the settings that require it -- About's
missing albums, a Discogs token beneath it:

```json
{"key": "missing", "label": "Missing albums", "kind": "toggle", "default": true},
{"key": "discogs_token", "label": "Discogs token", "kind": "secret",
 "requires": "missing", "link": "https://www.discogs.com/settings/developers"}
```

The toggle is told like any other setting, and so are those requiring it,
ticked or not: whether to use them is the program's to read from the toggle.

Every run is given them all, set or not, as `"settings": {"url":
"http://localhost:8080", "token": "…", "covers": true, "site":
"musicbrainz"}`. A secret is kept apart from Melody's other settings, in a
file only you can read (`plugin-secrets.conf` beside them).

The entries stand at the end of the context menu. With more than three of
them, from every plugin enabled, each plugin's go in a submenu of its own,
under its name, so the menu stays short however many there are.

## What the program is told

It runs in its own folder and gets the choice on stdin, as one JSON object:

```json
{
  "schema": 1,
  "action": "share",
  "engine": {"name": "gemenon", "address": "192.168.0.200:6603", "here": false},
  "settings": {"server": "https://share.example.org"},
  "albums": [
    {
      "artist": "Air Supply",
      "album": "Making Love… The Very Best of Air Supply",
      "date": "1983",
      "music_folder": "/srv/music",
      "folder": "/srv/music/Air Supply/1983 Making Love",
      "relative_folder": "Air Supply/1983 Making Love",
      "tracks": [
        {
          "path": "/srv/music/Air Supply/1983 Making Love/03 The One That You Love.mp3",
          "relative_path": "Air Supply/1983 Making Love/03 The One That You Love.mp3",
          "title": "The One That You Love",
          "artist": "Air Supply",
          "track_number": "3"
        }
      ]
    }
  ]
}
```

- Paths are the engine's: on a library elsewhere, the server's.
  `relative_folder` and `relative_path` are within the library's music
  folder, and empty for a file outside it.
- An album is its album artist, title and date; tracks without an album tag
  are an album of their folder.
- `engine` is where the files are; a plugin that wants more can ask it
  itself over Melody's protocol.

## What it answers

- **Success:** exit 0. What it printed on stdout, trimmed, is the result:
  notified, copied, or both, and shown in the status bar.
- **Failure:** any other exit. What it printed on stderr is shown in the
  status bar.
- It is stopped at its time limit, and when Melody closes.

### A list

An action with `"result": "list"` answers in JSON, and Melody shows it in a
window of its own -- a heading, then a row for each item, its detail beneath.
A row with a link opens in the browser on a double click; Copy puts the whole
list on the clipboard.

```json
{
  "title": "Testament: 12 missing",
  "items": [
    {"text": "The Legacy", "detail": "1987 · Album",
     "url": "https://musicbrainz.org/release-group/…"},
    {"text": "Souls of Black", "detail": "1990 · Album"}
  ]
}
```

`text` is the one key an item needs. A link is a web page's, `http` or
`https`: nothing else is opened from a plugin. The parts are Melody's to draw
-- text, detail, link -- and none of a plugin's own, so a list looks like the
rest of Melody; the same parts will fill a side panel later.

## Panels

A plugin can also keep a side panel current with what plays -- the artist's
missing albums, a biography, credits ([ADR-0291](adr/0291-plugin-panels.md)):

```json
"panels": [{"id": "artist", "label": "About the artist", "run": ["./about"]}]
```

A panel may hold controls ([ADR-0303](adr/0303-panels-with-controls.md)):

| Part | Keys |
| --- | --- |
| Input | `input` -- its key -- `label`, `value`, `placeholder`, `secret` (shown as dots), `action` (run on Enter) |
| Toggle | `toggle` -- its key -- `label`, `detail`, `on` |
| Button | `button` -- its text -- `action`, `value`, `role`; buttons side by side are a row |

and an item may carry `"buttons": [{"text", "action", "value", "on", "role"}]`
at its right -- a vote, one that is on drawn as chosen. Pressed, the panel's program
runs again, told `"action": {"id", "value"}` -- a toggle's key and `on` or
`off` -- and `"inputs"`, every input's text; its answer is the panel's new
content. What was typed stays unless the answer gives the input a new value.
A panel of controls says `"always": true` and is asked with nothing playing
too, `playing` then null.

Some parts say what they are, and Melody draws them as it draws its own
([ADR-0304](adr/0304-panels-drawn-as-melody.md)):

| Part | Keys |
| --- | --- |
| Track | `track` -- its title -- `artist`, `detail`, `cover` (an image's absolute path), `url`, `playing` (tinted in the accent), `buttons` |
| Status | `status` -- its text -- `detail`, `live` (a dot in the accent), `buttons` at its right |
| People | `people` -- a list of names, drawn as chips with coloured initials |
| Filters | `filters` -- badges in a row, `[{"text", "action", "value", "on"}]`, those on drawn as chosen; pressed, the panel runs again -- what it shows, and keeping the choice, are the plugin's |

A button's `role` is `primary` (the accent), `normal`, or `quiet` (flat; on a
row, shown only under the pointer). A row's or a status's button may open a
page, `"url"`, in place of an `action`. A toggle may say what it does beneath
its label, `"detail"`. `"primary": true` is the primary role. A
toggle is drawn as a switch. There is no CSS and no colour of a plugin's own:
a panel looks like the rest of Melody under every colour scheme.

A panel that shows a listening room says `"refresh": ["listening-room"]`,
and is asked again whenever the room changes -- a vote, a friend arriving --
not only with the track ([ADR-0302](adr/0302-listening-together.md)).

An answer still waiting for something -- images it fetches meanwhile --
says `"again_after": 3`, whole seconds from 1 to 60: the panel is asked
again about the same after so long, if it still shows, keeping what it
shows and where it was scrolled to until the new answer comes. Ten times
running at most; a new track starts again
([ADR-0312](adr/0312-a-panel-asks-to-be-asked-again.md)). About answers at
once with the covers it has kept, and asks again while the others are
fetched.

A plugin needs actions, panels, or both. Enabled, its panel is beside
Lyrics, shown and hidden with the other side panels by **Panels**
([ADR-0299](adr/0299-the-side-panels-show-together.md)). It asks only while
it shows: when the track changes, when it is shown, and on its refresh
button. Its program is given

```json
{"schema": 1, "panel": "artist", "engine": {…}, "settings": {…},
 "playing": {"path": "…", "relative_path": "…", "title": "…", "artist": "…",
             "album": "…", "album_artist": "…", "date": "…",
             "track_number": "…", "duration_ms": 250000,
             "tags": {"albumartist": ["Testament"],
                      "musicbrainzalbumartistid": ["05106775-…"], …}}}
```

`tags` is every tag the library has of the track
([ADR-0301](adr/0301-panels-are-told-the-library.md)), named as queries
name them -- a field Melody knows folded, lowercase without spaces, `_` or
`-` (`musicbrainzreleasegroupid`), another as it is spelled, lowercase --
each a list of its values. A file outside the library has none.

A panel that says `"library": ["artist-albums"]` is told, too, the playing
artist's albums in the library -- by album artist, else artist, and by
MusicBrainz id where the track has one; one for each release group, else
album and album artist; oldest first:

```json
"artist_albums": [{"album": "Low", "album_artist": "Testament", "date": "1994",
                   "folder": "/srv/music/Testament/Low", "relative_folder": "Testament/Low",
                   "tracks": 11, "tags": {"musicbrainzreleasegroupid": ["c28ee416-…"], …}}]
```

An album's `tags` are those every track of it shares. Melody asks the
engine for them; the plugin never needs to.

and answers what to show, in parts Melody draws:

```json
{"title": "Testament",
 "parts": [
   {"heading": "Missing albums"},
   {"text": "The Legacy", "detail": "1987 · Album", "url": "https://musicbrainz.org/…"},
   {"paragraph": "Thrash metal from Berkeley, California."},
   {"image": "/home/you/.cache/about/testament.jpg", "caption": "Live, 1988"}
 ]}
```

| Part | Keys |
| --- | --- |
| Heading | `heading` |
| Paragraph | `paragraph` |
| Item | `text`, and `detail` beneath, a `url` it opens |
| Image | `image` -- an absolute path of a file the plugin keeps -- and `caption`, `url`; as wide as the panel, no wider than it is |
| Link | `link`, its `url`, and `detail` -- what it is about, its tip; links side by side are a row of small buttons |

The title is the panel's heading; a heading opens a section; a paragraph
longer than six lines is folded, More beneath it. Links are `http` or
`https` -- or Melody's own `melody://together/…`, a vote at a listening
party (ADR-0302), which Melody opens itself. Melody fetches nothing for a panel: an image is
the plugin's to download and keep. `timeout` is 1 to 300 seconds, 30 by
default.

## Lyrics providers

When the files have no lyrics for what plays, Melody asks the enabled
lyrics providers in turn, and shows the first that finds them
([ADR-0293](adr/0293-lyrics-providers.md)):

```json
"providers": [{"id": "lyrics", "provides": "lyrics", "run": ["./lrclib"]}]
```

It is told what a panel is -- `playing`, `engine`, `settings` -- with
`"provide": "lyrics"`, and answers

```json
{"lyrics": "[00:12.00]You are my sweetest downfall\n[00:15.40]…", "source": "LRCLIB"}
```

or `{"lyrics": null}` when it has none. Timed lines (LRC) are synced and
followed as the music plays; text without timings is shown as it is. What a
provider finds is shown, never written into the files. `timeout` is 1 to 120
seconds, 30 by default. The official one, `lrclib`, comes with Melody.

## Triggers

A plugin can also run on its own -- when albums come into the library, when
it changes, every so often, when Melody starts, or from **Workspace › Run**
([ADR-0295](adr/0295-plugin-triggers.md)):

```json
"triggers": [{"id": "page", "label": "Write the music page", "run": ["./page"],
              "on": ["library-changed", "hand"]},
             {"id": "nightly", "on": "every", "every": 1440, "run": ["./backup"]}]
```

| Key | |
| --- | --- |
| `on` | One or more of `albums-added`, `library-changed` (once the library has been still half a minute), `every`, `start`, `hand`. |
| `every` | With `every`: minutes, 1 to 10080 -- or the key of a setting that holds them. |
| `label` | With `hand`: its entry under Workspace › Run. |
| `result` | `none` (the default), `notify` -- what it prints, as a notification -- `copy`, onto the clipboard, or `notify-and-copy` (ADR-0302). |
| `timeout` | 1 to 21600 seconds, 600 by default. |

Its program is given what started it, the engine holding the library and its
music folders -- and, for new albums, the albums, as an action gets them:

```json
{"schema": 1, "trigger": "page", "on": "library-changed", "engine": {…},
 "music_folders": ["/srv/music"], "settings": {…}}
```

One run of a trigger at a time: started again while it runs, it runs once
more after. Triggers run while Melody does; for the window closed, use
`melody-cli watch albums` or cron.

The official **Scripts** plugin needs no `plugin.json` of your own: a shell
command for each trigger, set under Settings → Plugins → Scripts. For each
new album it runs once, `{}` the album's folder and `MELODY_ALBUM_FOLDER`,
`_ARTIST`, `_TITLE`, `_DATE` set; every command is told `MELODY_TRIGGER`,
`MELODY_ENGINE`, `MELODY_MUSIC_FOLDERS` and -- the library on another engine
-- `MELODY_SERVER`, which melody-cli follows. A music page written whenever
the library changes:

```sh
melody-musiclist --output ~/public_html/index.html
```

## Example

[`examples/plugins/copy-folder`](../examples/plugins/copy-folder) is a whole
plugin to start from: a `plugin.json` and a dozen lines of Python that answer
with the album's folder, which Melody copies to the clipboard. Copy the folder
into `~/.local/share/melody/plugins/`, enable it, and change it.

To try a program by hand, give it what Melody would:

```sh
echo '{"schema": 1, "action": "copy-folder", "engine": {},
       "albums": [{"artist": "A", "album": "B", "date": "", "music_folder": "/srv/music",
                   "folder": "/srv/music/A/B", "relative_folder": "A/B", "tracks": []}]}' |
    ./copy-folder
```

## Publishing a plugin

A plugin is not part of Melody and does not need to be: give it a repository
of its own, and it is installed by putting its folder in the plugin folder --

```sh
git clone https://github.com/you/melody-albumshare ~/.local/share/melody/plugins/albumshare
```

-- then enabling it under Settings → Plugins. Give the repository the GitHub
topic **`melody-plugin`**, so it is found with the others. Say in its README
what it sends where: whoever enables it runs it with their rights.

`schema` 1 is what Melody promises: a plugin written for it keeps working, and
anything that changes the manifest or the JSON comes as a new schema, with 1
still read.

## Later

Providers of more than lyrics -- covers, biographies for the tagger -- are
for later, in the same form.

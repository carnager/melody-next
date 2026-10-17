# Plugins

A plugin adds an entry to the context menu of track lists and of the library:
"Share album", "Send to …". A few official ones come with Melody
([`plugins/`](../plugins): the web search); everything else is written and
published apart. A plugin is a program of its
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
it under the plugin in **Settings → Plugins**:

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
| `help` | Said when the pointer is over it. |

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

Plugins that stay running and answer Melody's questions -- lyrics from the
web, a side panel of information -- are designed in ADR-0288, not built yet.

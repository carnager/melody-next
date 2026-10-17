# ADR-0288: Plugins, first as actions

## Status

Accepted, 2026-10-17.

## Context

- **Wanted:** things Melody does not ship -- sharing an album through a server
  of one's own (`albumshare`), and later a side panel of information or
  lyrics from the web -- added by whoever wants them.
- **Nothing exists.** The roadmap puts plugins "later"; `MILESTONES.md` says
  not to stabilise a plugin ABI before the built-in boundaries have real use.
- **What albumshare needs** of Melody is little: the album's artist, title,
  date and folder relative to the library's music folder; the rest it does
  itself, talking to its server (and, if it likes, to the engine).

## Decision

1. **A plugin is a program apart**, in any language, that Melody runs and
   talks to in JSON. Melody loads no code: no ABI to freeze, no language
   chosen for anyone, and a plugin that fails cannot take the window down.
2. **A plugin is a folder** with a `plugin.json` in it:
   `~/.local/share/melody/plugins/NAME/`, or under `$XDG_DATA_DIRS`, where
   one of the user's of the same name wins. Melody ships a few **official**
   ones, under `/usr/share/melody/plugins/` -- only those that need nothing
   of anyone's own, no server, no account: the web search (2026-10-17).
   Everything else is written and published apart.
3. **It runs only when enabled**, under Settings → Plugins. One put there is
   found but off: a folder appearing does not run anything.
4. **Level 1, now: actions.** A plugin declares entries for the context menu
   of track lists and of the library:

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
         "run": ["./share"],
         "result": "notify-and-copy",
         "timeout": 300
       }
     ]
   }
   ```

   - `for`: `albums` (one run per album chosen) or `tracks` (one run with
     every track chosen).
   - `run`: the program and its arguments, without a shell; a first word
     starting with `./` is in the plugin's folder.
   - `result`: what its output is for -- `notify`, `copy`, `notify-and-copy`
     or `none`.
   - `timeout`: seconds before it is stopped (default 120).

   The entries stand at the end of the menu; with more than three in all,
   each plugin's go in a submenu under its name, as foobar2000 groups its
   components'. Where they go is not a setting.

   Melody runs it in the plugin's folder and writes on its stdin:

   ```json
   {
     "schema": 1,
     "action": "share",
     "engine": {"name": "gemenon", "address": "192.168.0.200:6603", "here": false},
     "albums": [
       {
         "artist": "…", "album": "…", "date": "…",
         "music_folder": "/srv/music",
         "folder": "/srv/music/Artist/Album",
         "relative_folder": "Artist/Album",
         "tracks": [
           {"path": "…", "relative_path": "…", "title": "…", "artist": "…",
            "track_number": "…"}
         ]
       }
     ]
   }
   ```

   Paths are the engine's, as text; `relative_*` is empty for a file outside
   the music folders. What it prints on stdout, trimmed, is its result; a
   non-zero exit is a failure, its stderr said. It runs off the UI thread and
   is stopped at its time limit or when the window closes.
5. **Later, designed here, not built:** a provider is a plugin that stays
   running and answers requests over the same JSON -- `lyrics.find` with the
   track's artist, title and duration, asked when Melody has none. Whether
   it runs in the window or in the engine (where lyrics live, so every
   client would have it) is decided then. A panel plugin gives content for a
   side panel in a fixed set of parts -- text, links, images -- that Melody
   draws: no HTML, no widgets of the plugin's own.

## Consequences

- A plugin is a few lines in any language; albumshare's is a small program
  reading the JSON and calling its server.
- Melody's only promise is the manifest and the JSON, versioned by `schema`.
- A plugin enabled runs with the user's rights, like any program they start;
  there is no sandbox.
- Actions cannot change what Melody shows beyond a notification, the
  clipboard and the status bar; that is what levels 2 and 3 are for.

## Verification

- A manifest is read with its every key, and one broken is said, not
  guessed at; a plugin is found and stays off until enabled.
- The JSON a plugin is given for tracks of a tab and for a library album:
  artist, album, date, folders and relative ones, the engine.
- A plugin's output becomes a notification and the clipboard; a failure and
  a time limit are said; nothing runs on the UI thread.

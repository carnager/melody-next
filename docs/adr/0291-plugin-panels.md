# ADR-0291: Plugin panels

## Status

Accepted, 2026-10-17. Builds ADR-0288's level 2, "a panel plugin", as it
was designed there; providers (its decision 5's other half) stay to come.

## Context

- **Actions answer once, when asked.** An artist's missing albums, a
  biography, credits, the next concert -- what a person wants beside what
  plays, kept current as it changes -- needs asking every track.
- **A list window already draws a plugin's answer** in Melody's own parts:
  text, detail, link (ADR-0288, 2026-10-17). A panel needs the same parts,
  and a few more.
- **Melody has panels:** Lyrics and Up Next, docks beside the lists, each
  toggled in a menu, each asking nothing while hidden.

## Decision

1. **A plugin may declare panels** in `plugin.json`, beside or instead of
   actions:
   `"panels": [{"id": "artist", "label": "About the artist", "run":
   ["./about"], "timeout": 30}]` -- `timeout` 1 to 300 seconds, 30 by
   default.
2. **A panel is a dock beside Lyrics**, toggled under Playback after it,
   remembering whether it is open and how wide, as Lyrics does.
3. **It asks about what plays, and only while open:** when the playing
   track changes, when it is opened, and when its refresh button is
   pressed. A newer question stops the run still answering the last; a
   hidden panel asks nothing.
4. **It is run as an action is** -- a program, in its folder, given JSON on
   stdin -- told:

   ```json
   {"schema": 1, "panel": "artist",
    "engine": {"name": "gemenon", "address": "…", "here": false},
    "playing": {"path": "…", "music_folder": "…", "relative_path": "…",
                "title": "…", "artist": "…", "album": "…", "album_artist": "…",
                "date": "…", "track_number": "…", "duration_ms": 250000},
    "settings": {}}
   ```

   `playing` is null when nothing plays; the plugin is not run then.
5. **It answers parts, and Melody draws them:**
   `{"title": "…", "parts": [...]}`, each part named by its one key --
   `{"heading": "…"}`, `{"paragraph": "…"}`, `{"text": "…", "detail": "…",
   "url": "…"}` (an item, as a list's), `{"image": "/abs/path", "caption":
   "…", "url": "…"}`. A link is `http` or `https`; an image is a file on
   this computer the plugin keeps -- Melody fetches nothing for it. No
   HTML, no widgets of the plugin's own: a panel looks like Melody's.
6. **The panels follow Settings:** a plugin enabled there brings its panels
   when Settings are saved, one turned off takes them.

## Consequences

- A plugin that wants the artist of what plays is a few lines more than an
  action: the same program, given `playing` instead of `albums`.
- Images are the plugin's to fetch and keep; a panel never makes Melody
  reach the network.
- Panels run in the window, as actions do: a phone or another window shows
  none of them.

## Verification

- `plugins`: panels read from a manifest or refused; an answer's parts
  read, links and images checked; what a panel is told, with nothing
  playing and with a track.
- `bench-main-window`: a panel's toggle after Lyrics, "Nothing is playing"
  when opened so, the parts of its answer for a track, gone when its
  plugin is turned off.

# ADR-0295: Plugin triggers

## Status

Accepted, 2026-10-19. Adds a fourth thing a plugin can declare beside
ADR-0288's actions, ADR-0291's panels and ADR-0293's providers.

## Context

- **People want their own programs run on what happens to the library**: a
  friend converting each new album for a player, a music page written again
  when the library changes (`melody-musiclist`), a backup every night.
- **`melody-cli watch albums` and cron do it from a shell** (protocol level
  18), but each is set up apart, by hand, outside Melody.
- **A plugin is already a program Melody runs with JSON on stdin** -- with
  settings, found and enabled in one place -- but only when something is
  chosen or plays.

## Decision

1. **A plugin may declare triggers**:
   `"triggers": [{"id": "page", "label": "Write the page", "on":
   ["library-changed", "hand"], "run": ["./page"], "result": "none",
   "timeout": 3600}]`. `on` is one or more of:
   - `albums-added` -- albums new to the library, each once whole
     (`catalogue.albums_added`, level 18);
   - `library-changed` -- the library changed, and has then been still for
     half a minute;
   - `every` -- every `every` minutes (1 to a week), or the minutes a setting
     of the plugin holds, when `every` names one;
   - `start` -- once when Melody starts;
   - `hand` -- an entry under **Workspace › Run**, named by `label`.

   `result` is `none` or `notify` (what it prints, as a notification);
   `timeout` 1 second to 6 hours, 10 minutes by default.
2. **Told what started it**: `{"schema": 1, "trigger", "on", "engine",
   "music_folders", "settings"}`, and for new albums `albums`, as an action
   is given them. The engine is the one holding the library (ADR-0286),
   whichever tab is in front.
3. **One run of each trigger at a time.** Started again while it runs, it
   runs once more after, with every album that came meanwhile. Saving
   Settings keeps what runs; a plugin turned off stops its runs.
4. **In the window, while it runs.** As every plugin (ADR-0288), triggers
   run in the desktop application, on this computer; what has to run with
   the window closed is `melody-cli watch albums` or a cron job.
5. **An official Scripts plugin** comes with Melody, off until enabled: a
   shell command for each trigger, set in its settings -- for each new album
   (`{}` its folder, `MELODY_ALBUM_FOLDER`, `_ARTIST`, `_TITLE`, `_DATE`),
   when the library changed, every so many minutes, at start, and by hand.
   Each is told `MELODY_TRIGGER`, `MELODY_ENGINE`, `MELODY_MUSIC_FOLDERS`,
   and `MELODY_SERVER` when the library is on another engine, which
   melody-cli follows.

## Consequences

- A script on the library's news needs no plugin of its own: a command in
  Scripts' settings is enough. A plugin of its own is for what has settings
  of its own or ships to others.
- Nothing runs while Melody is closed.
- A failure is said in the status bar and as a notification; a success is
  said only when run by hand, or when the trigger asks to notify.

## Verification

- `plugins`: triggers read with every key checked; an interval from a
  setting; what a trigger is told; the Scripts plugin runs the command set,
  once for each new album, says a failure, and by hand with none set says
  what to set.
- `bench-main-window`: a plugin's triggers run at start, from Workspace ›
  Run, once the library changed and was still -- asked again while running,
  once more after -- and for new albums; turned off, its entry goes.

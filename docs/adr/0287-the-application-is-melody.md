# ADR-0287: The application is Melody

## Status

Accepted, 2026-10-16.

Replaces the rename to "Melodies" decided on 2026-10-15, which was never
started. Keeps ADR-0220 (one engine, its clients) and ADR-0286 (one library).

## Context

Measured on 2026-10-16:

- **Two names for one product.** The engine family is Melody (`melodyd`,
  `melody-agent`, `melody-cli`, `melody-watch`, `~/.config/melody/`,
  `_melody._tcp`); the desktop client is Trackknife. Since ADR-0220 the engine
  is the product and the window one of its clients, beside the phone and the
  command line.
- **What users see of "Trackknife"** is small: about 60 strings (the window
  title, the application display name, notifications, MPRIS' identity,
  dialogs, `melody-cli --help`), the `trackknife` binary and desktop file, the
  `trackknife-git` package, the PipeWire application id and two User-Agents,
  MusicBrainz' among them.
- **Persistent names.** Qt's organisation and application are `trackknife`
  (`src/bench/main.cpp:115`): settings in `~/.config/trackknife/`, data in
  `~/.local/share/trackknife/trackknife`, cache in
  `~/.cache/trackknife/trackknife`. MPRIS is `org.mpris.MediaPlayer2.trackknife`,
  the desktop entry `trackknife`.
- **melodyd keeps its state in the window's folder.** Its default is
  `$XDG_DATA_HOME/trackknife/trackknife` (macOS
  `~/Library/Application Support/trackknife`), chosen so the engine would
  adopt the data Trackknife had (`src/daemon/main.cpp:123`); the launcher
  holds a copy of the path and calls an engine the shared one by it
  (`src/bench/engine_launcher.cpp:52`). On caprica that folder holds the
  engine's database beside the window's tabs and backups; on gemenon, a
  server, only the engine's.
- **Internal names**: about 7,000 -- the `trackknife::` namespace, include
  paths, CMake targets and aliases. 53 `TRACKKNIFE_*` names are CMake options,
  test and runtime environment variables.
- **Written into files and exports**: the rating backup tag, when turned on
  by default `TRACKKNIFE_RATING` (ADR-0245), the format names of tagging-script and
  encoder-preset files, the embedded-artwork hash domain.
- Nothing is released (no compatibility owed to old names), but three people
  use it: their settings and data move.

## Decision

1. **The application is Melody.** Every name a user sees says Melody: the
   window title, notifications, MPRIS' identity, dialogs, help, docs. The
   binary is `melody`, the desktop file `melody.desktop`, MPRIS
   `org.mpris.MediaPlayer2.melody`, the PipeWire application id and the
   User-Agents Melody's. Its icon is the vinyl bird -- a blackbird whose
   body is a record (`packaging/icons/melodies-vinyl-bird-v7.svg`) --
   replacing the crate icons; the blackbird
   (`packaging/icons/melodies-blackbird-v2.svg`) is its mascot, shown in a
   new **Help → About Melody**: the version and commit, the licence
   (GPL-3.0) and links to the project and its documentation.
2. **One folder per kind, named melody.** Qt's organisation and application
   become `melody`: settings in `~/.config/melody/melody.conf`, beside
   `melodyd.conf`; the window's data in `~/.local/share/melody/melody`, its
   cache in `~/.cache/melody/melody`. **melodyd's state** moves to
   `~/.local/share/melody/engine` (macOS
   `~/Library/Application Support/melody/engine`); `--state` and Docker's
   `/state` are unchanged.
3. **Moved once.** At its first start the window moves its settings, data
   and cache from the `trackknife` names, as Trackbench's were moved before
   (`src/workspace/startup.cpp:19`). melodyd, at its first start with no
   state at the new place, moves **its own files** -- its database, id,
   password and token, keys, staging and undo copies -- from the old folder
   before opening anything, under its lock; what else is there (the window's
   tabs and backups) is the window's to move. A move that fails leaves
   everything where it was, and the engine says so and starts from the old
   folder.
4. **Packages.** `melody-git` becomes the application, depending on
   `melodyd-git`; it replaces `trackknife-git`. The metapackage it was goes.
5. **Variables are renamed, without the old names.** Every `TRACKKNIFE_*`
   CMake option and environment variable becomes `MELODY_*`
   (`MELODY_STATE_DIR`, …); nothing released reads the old ones.
6. **The rating backup tag is `MELODY_RATING`.** The copy of each rating in
   a tag of the user's naming (ADR-0245), when it is turned on, is named
   `MELODY_RATING` unless named otherwise. `TRACKKNIFE_RATING` stays only for
   compatibility: someone who had the copy on under that old default keeps
   it as their own name, set once as their settings move, so no file is
   written anew. The format names of exported tagging scripts and encoder
   presets and the artwork hash domain stay: renaming them would make old
   exports unreadable or change every hash.
7. **Internal names stay**: the `trackknife::` namespace, include paths,
   targets and test names. They are renamed opportunistically, as Trackbench's
   are (AGENTS.md).

## Consequences

- One name to install, find and ask about; the phone app is Melody too.
- A user's settings, tabs and the engine's library move once, at the first
  start of each; a server's engine moves without the window.
- Two releases of the window cannot share a settings folder across the move;
  none is released, so none needs to.
- "Melody" is a common word: harder to search for than Trackknife.
- Code reads `trackknife::` for a product called Melody until it is renamed.

## Verification

- A home with only the old folders: after the first start of melodyd and of
  the window, the library, ratings, history, tabs and settings are the same,
  the old folders hold nothing of theirs, and a second start moves nothing.
- melodyd alone (a server): its files move, nothing else is touched.
- A move that cannot complete -- the new folder not writable -- leaves the
  old folder as it was and the engine running from it.
- The desktop entry, the window and the About box show the vinyl bird at
  16 px and up; About shows the blackbird, the version and the licence.
- No user-visible string, desktop entry, MPRIS name or package says
  Trackknife; `grep -r TRACKKNIFE_` finds only the compatibility name.
- A backup copy turned on now is `MELODY_RATING`; one that was on under the
  old default still writes `TRACKKNIFE_RATING`, and no file is written for
  the change (tested in `trackknife-names`).

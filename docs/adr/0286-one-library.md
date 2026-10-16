# ADR-0286: One library, one Engines page

## Status

Accepted, 2026-10-16.

Amends ADR-0227 and ADR-0234 (engines side by side, each with a library) and
ADR-0233 and ADR-0259 (working lists live on the engine). Keeps ADR-0220's
single playback engine, ADR-0223's passwords, ADR-0228's agents and ADR-0237's
file work in the engine.

## Context

Measured on 2026-10-16:

- **Two places define a library.** Settings → Library holds this computer's
  engine's music folders. Settings → Engine holds this engine's sharing (music
  root, listen address, password, playing for others) beside "Engines
  elsewhere", each with an address, password, music folder, mount and stream
  rates (`src/bench/settings_dialog.cpp:298-560`).
- **Libraries in practice: one.** caprica's engine has no music folders
  (`catalogue.roots` → `[]`); the library is gemenon's. The three users each
  have one library.
- **Working tabs live on the engine** (ADR-0233/0259), so `melody-cli lists`
  shows every client's open tabs ("gemenon · working" eight times), and the
  window keeps a sync, drafts and a cache to mirror them.
- **Setting up a remote engine** means editing an environment file and
  writing a password file on the server. Without a password the engine runs
  on its machine only (`src/daemon/main.cpp:583`), its local socket trusted
  without one.
- **One password is already the assumption**: `--play-for` and `--agent`
  default to the engine's own (`main.cpp:442`), and the remote's password
  field says "only when that engine's password differs from yours".
- **Speakers are not engines.** `melody-agent` and the phone connect to an
  engine and offer it an output (ADR-0228).

## Decision

1. **One library**, on one engine the user chooses: this computer's or a
   remote one. This computer's engine is always there for this computer's own
   files and playback; it has no library unless it is the library.
2. **One password for all engines.** Trackknife keeps one; the per-engine
   password fields go. The command-line flags stay for unusual setups.
3. **One Engines page** replaces Settings → Library and Settings → Engine:
   the password, the library engine with its status and its music folders
   (chosen with the engine folder browser), and this computer's "let the
   library play on this computer's speakers" (`--play-for`). Mounts, stream
   rates and the listen address go under Advanced. A library on this computer
   listens on the network with the password; there is no separate sharing
   switch.
4. **Only saved playlists live on the engine.** Tabs are Trackknife's: it
   keeps their rows itself, and a tab plays by sending its rows as the queue
   (`playback.replace_queue`, already the client's fallback). The phone and
   `melody-cli` see saved playlists only. Working lists and drafts leave the
   engine by a reversible migration.
5. **One queue**, on the engine that plays, as now; moving it to another
   speaker is `outputs.select`. One listener does not need a queue per
   client.
6. **No migration for two libraries**: none of the three users has one.

### Setup

- **On the desktop**, nothing: Trackknife starts its engine and gives it the
  password, as it does now.
- **On a server**, after installing, the password is given there once:

  ```
  $ melody-cli setup
  Password for your engines: ********
  Again: ********
  gemenon is on the network at 192.168.0.200:6603.
  ```

  It runs over the local socket, which is trusted already (`docker exec -it
  melodyd melody-cli setup` in Docker). The engine keeps the password in its
  state folder, reads it from there when no `--password`/`--password-file` is
  given, and goes on the network at once, without a restart. Over TCP the
  command is refused. Run again, it changes the password.
- **Everything else is in Trackknife.** The engine is found on the network;
  connecting uses the shared password. An engine with no music folders says
  so on the Engines page and offers to choose them; the first scan starts when
  they are chosen.
- Changing the password in Trackknife sends the new one to every engine it
  reaches (`engine.set_password`, authenticated with the old one).

## Left for later

- How tabs of this computer's files and tabs of the library differ, and what
  a drop between them does.
- Moving the library to another engine, with its playlists, ratings and
  history.

## Alternatives considered

- **Every engine alike, tabs mixing their files** (draft of 2026-10-16,
  parked): needs one engine streaming another's files, and every file tool,
  rating and listen split by row.
- **Setting up a fresh engine entirely from Trackknife**, with the first
  client claiming it or an accept on the server: nothing to type on the
  server, but whoever comes first owns the music files, or the accept splits
  one action across two places.
- **Password and music folders both in `melody-cli setup`**: the server is
  finished in one go, but the folders are then set in a second place besides
  the Engines page.
- **A queue per client**, held by the library engine: measured, nothing on the
  wire says who asks, one `Player` is handed to about fourteen parts of the
  engine, and outputs are engine-wide -- much work for a second listener there
  is not.

## Consequences

- One place to set up the library, wherever it lives; a server needs one
  command and no files edited.
- `melody-cli lists` and the phone show playlists, not open tabs.
- A second library, or a second listener with their own queue, cannot be
  expressed in the UI.

## Stages

1. `melody-cli setup` and the engine's own password.
2. The Engines page and the one password.
3. Tabs in Trackknife; working lists leave the engine.

## Verification

- An engine without a password takes one from `melody-cli setup` over its
  socket, refuses the command over TCP, is on the network at once, and keeps
  the password across a restart; `--password-file` still wins.
- The Engines page connects to a freshly set up engine, chooses its folders
  and starts the first scan, in a two-engine test.
- After stage 3, `list.all` holds saved playlists only, and tabs survive a
  restart from Trackknife's own store; the migration is undone cleanly.
- Live: gemenon set up from scratch in a test state folder (systemd drop-in)
  with one `melody-cli setup` and Trackknife.

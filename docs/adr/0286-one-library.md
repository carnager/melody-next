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
   (chosen with the engine folder browser). Mounts, stream choices and the
   listen address go under Advanced. A library on this computer listens on the
   network with the password; there is no separate sharing switch.
4. **Only saved playlists live on the engine.** Tabs are Trackknife's: it
   keeps their rows itself, and a tab plays by sending its rows as the queue
   (`playback.replace_queue`, already the client's fallback). The phone and
   `melody-cli` see saved playlists only. Working lists and drafts leave the
   engine by a reversible migration.
5. **One queue**, on the engine that plays, as now; moving it to another
   speaker is `outputs.select`. One listener does not need a queue per
   client.
6. **No migration for two libraries**: none of the three users has one.
7. **Every engine plays for every other** by default (`agent = yes` in
   melodyd itself, not only when Trackknife starts it) -- when it has a
   password, for without one no other engine would take it. An engine with
   speakers -- headless or not -- is a speaker for the others; newest wins
   (ADR-0229). An engine with no audio device offers nothing.

### melodyd runs as a service; Trackknife only connects

On Linux Trackknife no longer starts melodyd. The package ships
`melodyd.service` and `melodyd.socket`, the socket enabled for every user
(`/usr/lib/systemd/user/sockets.target.wants`): the first connection to
`$XDG_RUNTIME_DIR/melodyd.sock` starts the engine (socket activation, read
from `LISTEN_FDS` without libsystemd). When nothing answers, Trackknife
starts the socket if it is enabled and not running yet -- just installed --
and otherwise says what starts it. The desktop engine is then configured and
run exactly as a server's; `engine.info` says `service` when a service
manager runs it.

On macOS, and for an engine of another folder (a test's, a second
workspace's), Trackknife still starts the engine when nothing answers -- as
`melodyd` with its socket, folder and file, no other flag. Every setting is
in `melodyd.conf` there too.

Restarting is the service manager's: `engine.restart` exits and systemd
(`Restart=always`) or Docker starts it again; one Trackknife started is
stopped and started by it. `engine.stop` -- Quit and stop engine -- exits
with 64, which the unit does not restart after
(`RestartPreventExitStatus=64`); the next client on the socket starts it.

Trackknife's sharing settings become the engine's: `config.get`/`config.set`
read and change its `melodyd.conf` over the protocol, all or none, checked as
melodyd reads it. Settings' sharing is handed over once, when the window
first meets the engine, and on every save, the engine restarting to use it.
An engine an older Trackknife started is stopped and `melodyd.socket`
started, once, when nothing plays: the one time Trackknife starts anything of
systemd's.

### `melodyd.conf`

One file configures an engine, `~/.config/melody/melodyd.conf` by default,
another with `--config FILE` or `$MELODYD_CONFIG` (Docker sets
`/state/melodyd.conf`; the test suite one of its own, so no test reads or
writes a user's). It takes over
the name of today's environment file; a file still holding
`MELODYD_OPTIONS=…` is converted once into keys.

```
# melodyd.conf -- caprica

name = caprica
password = ********

# How this machine plays for every engine.
agent-music-root = /mnt/nas/Music/Rips/flac
agent-stream-home = original
agent-stream-away = opus 128

[engine gemenon]
# Its address, for when the network can't find it (WireGuard).
address = 192.168.0.200:6603
```

- **Keys are the flag names** without `--`; switches are `yes`/`no`; a flag
  given on the command line wins. Comments are lines of their own, above
  what they explain; blank lines are ignored.
- **`[engine NAME]`** is another engine this one plays for, by the name it
  announces: `address` (connected only while the network does not find it),
  `music-root`, `stream-home`, `stream-away`, `password`, `agent` (`no`
  leaves it out). The top-level `agent-*` keys are the defaults for every
  engine.
- **Renamed:** `--play-for` and its options become sections;
  `--agent-bitrate-nearby`/`-away` become `agent-stream-home`/`-away` with
  `original` or `opus N`. They are the agent's wish (ADR-0239), so they live
  only where the agent is. `melody-agent` takes the same names.
- **Strict:** an unknown key, section or value, or a key given twice, stops
  the start with file and line.
- **Values** are the rest of the line, trimmed, as raw bytes: paths need not
  be UTF-8. No quoting.
- **The password** is a key; the file is written readable by its owner only,
  and the engine warns when others can read it. `password-file` stays for a
  password kept apart; both at once is an error.
- **Changes take effect on restart.**

### Setting up an engine: edit the file

A new file starts from a commented skeleton -- every option, commented out,
with what it does above it, installed as `/usr/share/melody/melodyd.conf` and
built in -- when `melody-cli config` or `config.set` first writes one;
melodyd itself never writes it at start. A new key goes below its example
there. `melody-cli config --edit` opens it in `$EDITOR`, checks the file with the engine's
parser when it is saved, and offers to edit again on a mistake.
`melody-cli config KEY [VALUE]` and `--unset` read and change one key
(`--engine NAME` for a section), changing that line only, keeping comments and
order, writing atomically and through a symlink.

On a server that is: install, set `password` in the file, restart. An engine
without a password stays on its machine, as now (ADR-0223). Its music folders
are chosen in Trackknife, which finds it on the network.

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
- **`melody-cli setup` asking for the password**, the engine going on the
  network by restarting itself in place: one more command, where the
  skeleton already says everything the questions would.
- **Trackknife starting its own engine**, as now: about 500 lines of
  launcher building flags and password files, and a desktop engine that is
  configured unlike every other.
- **A queue per client**, held by the library engine: measured, nothing on the
  wire says who asks, one `Player` is handed to about fourteen parts of the
  engine, and outputs are engine-wide -- much work for a second listener there
  is not.

## Consequences

- One place to set up the library, wherever it lives; every engine, the
  desktop's included, is one config file and a service.
- caprica's engine state moves from Trackknife's folder
  (`~/.local/share/trackknife/trackknife`) to the service's, once, keeping
  its history, ratings and lists.
- Without systemd or launchd, melodyd is started by hand.
- `melody-cli lists` and the phone show playlists, not open tabs.
- A second library, or a second listener with their own queue, cannot be
  expressed in the UI.

## Stages

1. `melodyd.conf`: the parser (core, shared), melodyd reading it, the
   renames, `agent` on by default, the skeleton, `melody-cli config`.
2. Socket activation and the units; Trackknife connects instead of starting
   the engine, and caprica's state moves once.
3. The Engines page and the one password.
4. Tabs in Trackknife; working lists leave the engine.

## Verification

- The parser: every key, sections, switches, raw-byte paths, and each
  refusal with its line; writing one key keeps comments, order, permissions
  and a symlink.
- melodyd started with a file behaves as with the same flags; a flag wins
  over the file; an old `MELODYD_OPTIONS` file is converted once.
- `melody-cli config --edit` refuses a broken file before the engine sees it.
- An `[engine]` address is used only while that engine is not found, and
  never makes a second connection to it.
- After stage 2, caprica's history, ratings and lists are the same before and
  after the state moves.
- The Engines page connects to a freshly set up engine, chooses its folders
  and starts the first scan, in a two-engine test.
- After stage 3, `list.all` holds saved playlists only, and tabs survive a
  restart from Trackknife's own store; the migration is undone cleanly.
- Live: gemenon set up from scratch in a test state folder (systemd drop-in)
  with its skeleton, one password line and Trackknife.

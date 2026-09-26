# Engines, agents and the phone

```
  Trackknife ──┐                         ┌──► its own speakers
  phone app  ──┼──►  melodyd (engine)  ──┼──► agent: another computer, a Pi
  melody-cli ──┘     library, queue,     └──► agent: the phone
                     history, Last.fm
```

- **The engine**, `melodyd`, owns the library and playback: the queue,
  Up Next, ratings, listening history and scrobbling. It runs whether or not
  anything is connected to it.
- **Clients** control it: Trackknife, the Android app, `melody-cli`. They
  hold nothing of their own; close them and the music goes on.
- **Outputs** are where the sound comes out: the engine's own speakers, or
  an *agent*, which is any machine or phone that offers its speakers to
  engines. You pick one output at a time, and can switch mid-track.

## Finding each other

Engines announce themselves on the local network by name (multicast DNS), so
clients and agents find them without addresses. Where that doesn't reach,
over a VPN say, give the address instead: `HOST:6603`.

Every connection over the network gives the engine's password. Use one
password for all your engines and agents: an engine playing for another, or
lending its speakers with `--agent`, uses its own unless told otherwise.

## Headless melodyd

On a desktop, Trackknife starts its engine for you. On a server or a Pi, run
it yourself, as a user service:

```sh
# ~/.config/melody/melodyd.conf
MELODYD_OPTIONS=--name myserver --password-file /home/you/.config/melody/password
```

```sh
printf '%s\n' 'your password' > ~/.config/melody/password
chmod 600 ~/.config/melody/password
```

```sh
systemctl --user enable --now melodyd
loginctl enable-linger $USER
```

or with Docker ([Installation](install.md#docker)). Useful options:

| Option | What it does |
| --- | --- |
| `--name NAME` | What clients see it as. Default: the host name. |
| `--password-file FILE` | The password every network connection must give. Without one the engine stays on this machine. |
| `--local-only` | No network at all, just the local socket. The default without a password. |
| `--music-root DIR` | Where its music is, so agents with their own copy can open the files directly (see below). |
| `--agent` | Also lend this machine's speakers to other engines on the network. |
| `--agent-music-root DIR` | Where those engines' music is mounted here. |
| `--transcode-cache MB` | Space for converted tracks. Default 2048. |

With a password it listens on port 6603 for clients and agents, 6604 for
streams, and 5353/udp for multicast DNS. There's no encryption, so keep these
ports off the internet and use a VPN from outside -- or, if you want TLS, put
a proxy (stunnel, an nginx or Caddy stream proxy) in front of an engine
listening on `127.0.0.1`. `melodyd --help` lists everything.

## A NAS that can't run the engine

When the music is on a NAS and the engine runs on another machine, the engine
reads the files over the network mount, and a scan walks the whole library
across it. `melody-watch` runs on the NAS instead and tells the engine which
files changed, so it re-reads just those.

```sh
melody-watch --server myserver:6603 --password-file ~/.config/melody/password \
    /volume1/music=/mnt/nas/music
```

Each folder is `WHERE_IT_IS_HERE=WHERE_THE_ENGINE_SEES_IT` (one path if they
are the same), and the engine's library must include the second. At start it
compares the folder with the engine's library, so changes made while it wasn't
running are found without a scan. It needs only `libutf8proc`: build it on the
NAS with `cmake -DTRACKKNIFE_CLI_ONLY=ON`, which also builds `melody-cli`.
There's a user unit, `melody-watch.service`, reading its options from
`~/.config/melody/watch.conf` (`MELODY_WATCH_OPTIONS=...`).

A large library may need more inotify watches than the default; the watcher
says so, and `sysctl fs.inotify.max_user_watches=524288` raises the limit.

## Agents

There are three kinds:

- **Another engine** started with `--agent` (in Trackknife: **Settings →
  Engine → Let other engines play on this computer's speakers**). Use this on
  a machine that runs an engine anyway.
- **`melody-agent`** on a machine that only plays: a Pi next to the stereo.
  Without `--server` it plays for every engine it finds; with
  `--server HOST:6603` only for that one.
- **The phone**, which offers itself unless you turn it off in the app.

When two engines want the same speakers, the one that started playing last
gets them and the other pauses.

**Files or streams.** An agent that can see the music itself (the same NFS
mount, say) opens the files directly: start the engine with `--music-root`
and the agent with `--music-root` (or `--agent-music-root`) pointing at its
own copy. Otherwise the engine streams the tracks. The phone on mobile data
gets Opus instead of the original files.

## melody-cli

For scripts, key bindings and status bars. It finds the engine like the
others do: `--server`, `$MELODY_SERVER`, the one on this machine, or one on
the network (`--engine NAME` if there are several).

```sh
melody-cli status
melody-cli current                    # "Artist - Title", or nothing when stopped
melody-cli current --format '%artist% — %title% \(%playback_time%/%length%\)'
melody-cli find 'genre IS jazz SORT BY %date%' --format '%date% %artist% - %title%'
melody-cli play album doors 1967      # every word must match
melody-cli next track riders storm    # plays after the current track
melody-cli lists                      # the engine's lists, saved and working
melody-cli play list road trip        # one of them, by name or its words
melody-cli output "Pixel 10 Pro"
melody-cli rate 4                     # stars for what plays, 0-5
melody-cli love                       # on Last.fm, with the engine's account
melody-cli --json watch               # a line each time something changes
melody-cli watch --all                # the same, for whichever engine plays
```

`current` and `find` print tracks as the engine formats them, with a
[tkfmt-1](title-formatting.md) expression given by `--format` -- the language
Trackknife formats with. The engine has each track's whole library row, so
every tag is a field (`%replaygain_track_gain%`, `%genre%`, `%composer%`, ...),
the technicals come through `$info(codec)`, `$info(samplerate)`,
`$info(bitspersample)`, `$info(channels)`, and `%path%`, `%length%` and
`%rating%` (1-10, half stars; absent when unrated) are there too. `current`
adds `%playback_state%`, `%playback_time%` and `%playback_remaining%`.
`find` takes a [Trackknife query](query-language.md) -- plain words, or
`artist IS "Alice in Chains" SORT BY $num(%tracknumber%,2)` -- and prints a
line for each track, in the query's order; `--keys` adds a tab and each
track's key for `add --key`. There are no
optional `[...]` sections; `$if(%date%, \(%date%\))` does that, and a literal
`(`, `)`, `,`, `$` or `%` is escaped with a backslash. Stars:

```sh
melody-cli current --format '%artist% - %title%$if(%rating%, $repeat(★,$div(%rating%,2))$if($mod(%rating%,2),½))'
# Songs: Ohia - Farewell Transmission ★★★½
```

There's a bar widget for DankMaterialShell built on these in
[`packaging/dms/melody`](../packaging/dms/melody/), and
[`packaging/rofi/melody-rofi`](../packaging/rofi/melody-rofi) picks an album (or,
with `melody-rofi tracks`, a track) in rofi and puts it in Up Next, adds it to
the list that plays, or replaces that list and plays it (`melody-rofi tracks
--engine gemenon` for a particular engine).

## When something doesn't work

- **The engine isn't found by name.** Multicast DNS needs 5353/udp between
  the machines and doesn't cross a Docker bridge (use host networking).
  Connecting by address always works.
- **"Could not play: …"** is the engine's own reason: an output it couldn't
  open, a file it couldn't read.
- **An output shows as offline.** Check the agent's log. Each line names the
  engine it's about.
- **Converted tracks** are kept in `transcodes/` in the engine's state
  directory. Deleting that folder is safe.

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

## Headless melodyd

On a desktop, Trackknife starts its engine for you. On a server or a Pi, run
it yourself, as a user service:

```sh
# ~/.config/melody/melodyd.conf
MELODYD_OPTIONS=--name myserver
```

```sh
systemctl --user enable --now melodyd
loginctl enable-linger $USER
```

or with Docker ([Installation](install.md#docker)). Useful options:

| Option | What it does |
| --- | --- |
| `--name NAME` | What clients see it as. Default: the host name. |
| `--password-file FILE` | Every connection must give this password. Without one, anyone on your network can control it. |
| `--local-only` | No network at all, just the local socket. |
| `--music-root DIR` | Where its music is, so agents with their own copy can open the files directly (see below). |
| `--agent` | Also lend this machine's speakers to other engines on the network. |
| `--agent-music-root DIR` | Where those engines' music is mounted here. |
| `--transcode-cache MB` | Space for converted tracks. Default 2048. |

It listens on port 6603 for clients and agents, 6604 for streams, and
5353/udp for multicast DNS. There's no encryption, so keep these ports off
the internet and use a VPN from outside. `melodyd --help` lists everything.

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
melody-cli play album doors 1967      # every word must match
melody-cli next track riders storm    # plays after the current track
melody-cli output "Pixel 10 Pro"
melody-cli rate 4                     # stars for what plays, 0-5
melody-cli love                       # on Last.fm, with the engine's account
melody-cli --json watch               # a line each time something changes
melody-cli watch --all                # the same, for whichever engine plays
```

There's a bar widget for DankMaterialShell built on these in
[`packaging/dms/melody`](../packaging/dms/melody/).

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

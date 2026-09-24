# Melody: the engine, speakers and the phone

Melody is the part of Trackknife that plays music. It is one program,
`melodyd`, and it owns the library and playback: the queue, Up Next,
ratings, listening history, Last.fm. Everything else is a way of talking to
it:

- **Trackknife**, the desktop app, for browsing, playlists and file work.
- **The Android app**, a remote control that can also be a speaker.
- **`melody-cli`**, for scripts, key bindings and status bars.

Music comes out of *outputs*: the engine's own speakers, or any other
machine or phone that has registered with it as an *agent*. You pick one,
and you can move the music between them without it stopping.

## On one computer

Nothing to set up. Trackknife starts its own engine when it starts, and the
engine keeps running when you close the window, so the music doesn't stop.
Add your music with **Folders…** in the library panel and press **Refresh**.

## A server

Run `melodyd` on the machine that holds the music, a NAS or a home server,
and use it from everywhere else.

**Arch Linux.** `packaging/arch/PKGBUILD` builds split packages; on a server
you only need `melodyd-git`:

```sh
cd packaging/arch
makepkg -s
sudo pacman -U melodyd-git-*.pkg.tar.zst melody-cli-git-*.pkg.tar.zst
```

Put its options in `~/.config/melody/melodyd.conf`:

```sh
MELODYD_OPTIONS=--name gemenon --agent
```

and start it:

```sh
systemctl --user enable --now melodyd
loginctl enable-linger $USER    # so it runs without you logged in
```

**Docker.** `packaging/docker/docker-compose.yml` runs the engine with host
networking, which it needs to be found by name. Change the music path and
the name, then:

```sh
docker compose -f packaging/docker/docker-compose.yml up -d
```

The music is mounted read-only. The engine never writes to your files;
tagging happens in Trackknife over a network share (see below).

**Adding the music.** In Trackknife, go to **Settings → Engine**, choose the
server under **Remote engine → On the network**, then add its music folder
with **Folders…** in the library panel, as the server sees it (`/music` in the Docker
setup) and press **Refresh**.

**What it listens on.** Port 6603 for clients and agents, 6604 for streams,
and multicast DNS (5353/udp) so it can be found by name. `--local-only`
turns the network off. `--password-file FILE` makes every connection give a
password first; without one, anyone on your network can control it. There is
no encryption either way, so keep these ports off the internet (see
[Away from home](#away-from-home)).

## Speakers

The engine plays on the speakers of the machine it runs on. To play
somewhere else, that machine or phone has to offer itself as an output.
There are three ways to do that:

- **Another engine.** If the other machine runs `melodyd` anyway (a
  desktop with Trackknife, say), start it with `--agent`, or tick
  **Settings → Engine → Let other engines play on this computer's
  speakers** in Trackknife. It then plays for every engine it finds on the
  network. You don't need a separate agent on a machine that already has
  an engine.
- **`melody-agent`** on a machine with no engine: a Pi next to the stereo,
  for example. Without `--server` it plays for every engine it finds; with
  `--server HOST:6603` only for that one.
- **The phone**, see below.

When two engines want the same speakers, the one that started playing last
gets them and the other pauses. Trackknife shows who took them.

**Files or streams.** An agent that can see the music, because it's on the
same NFS mount for example, opens the files itself. Tell it where:
`--agent-music-root /mnt/music` (or `--music-root` for `melody-agent`), with
the engine started with `--music-root` pointing at its own copy. Without
that, the engine streams the tracks to the agent. That's fine on a LAN; a
phone on mobile data gets them as Opus instead.

**Choosing where it plays.** Trackknife's output menu in the header, the
speaker button in the phone app, or `melody-cli output NAME`. The music moves
to the new output at the same position.

## The phone

The Android app is in `android/`. Build it with:

```sh
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

It finds engines on your network by name. **Or by address** takes a host
and port instead, for when you're connected over a VPN. After that it's a
remote control: the library, search, the queue and Up Next, ratings, and
the media controls on the lock screen.

**The phone as a speaker.** By default the phone registers with the engine
under its model name, so it shows up as an output next to everything else.
Pick it and the engine plays there, gapless and with ReplayGain. You can
turn this off or rename the phone in the app's settings.

**Mobile data.** On Wi-Fi the phone gets the original files. On a metered
network it asks for Opus instead (128 kbps unless you change it), and that
applies from the next track. Formats Android can't play, like WavPack, APE,
tracker files and single tracks from a CUE sheet, always arrive as Opus.

**Albums on the phone.** The download button on an album page keeps it on
the phone, as Opus 160 by default and on Wi-Fi only unless you change that.
Kept albums are listed under **On this phone** in the library and play
without the engine. If the engine can't be reached, the library says so and
takes you there. When the engine plays a track the phone has kept, the phone
plays its own copy instead of streaming it.

## Away from home

Use a VPN (WireGuard, Tailscale or similar) and connect to the engine's VPN
address. The engine has no TLS, so don't forward its ports to the internet.
Over a VPN the phone counts as on mobile data if the underlying network is
metered, so streams stay small.

## Scripting

`melody-cli` finds the engine the same way the others do: `--server`,
`$MELODY_SERVER`, the one on this machine, or one on the network (pick it
with `--engine NAME` if there are several).

```sh
melody-cli status
melody-cli play album doors 1967      # every word must match
melody-cli next track riders storm    # plays after the current track
melody-cli latest 10
melody-cli output "Pixel 10 Pro"
melody-cli --json status | jq .position_ms
```

A command that fails exits non-zero and says why. `--json` prints the
engine's own answers, which is what to use in scripts. Anything the CLI
doesn't cover is one line of JSON away: the protocol is readable with `nc`.
`melodyd --help` shows an example.

## Editing files from a server library

The engine doesn't write tags. Trackknife does, on files it can open. Mount
the server's music on your desktop (NFS, sshfs) and tell Trackknife where,
under **Settings → Engine**: **Remote music folder** is the path as the
engine sees it, **Mounted here at** is where it is on your machine. Then
**Tools → Edit tags…**, **ReplayGain…** and **Convert files…** work on server
tracks, and the engine picks the changes up.

## When something doesn't work

- **The engine isn't found by name.** Multicast DNS needs 5353/udp between
  the machines, and doesn't cross a Docker bridge (use host networking).
  Connecting by address always works.
- **"Could not play: …"** in Trackknife or the phone, or an error from
  `melody-cli`, is the engine's own reason: an output that couldn't be
  opened, a file that couldn't be read.
- **An output shows as offline.** Check the agent's log. Every line names
  the engine it's about, e.g.
  `melody-agent: 192.168.0.13:6603: the engine went away; reconnecting`.
- **Converted tracks** are kept in `transcodes/` in the engine's state
  directory, 2 GB by default (`--transcode-cache MB`). Deleting the folder
  is safe.

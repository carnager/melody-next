# Using Trackknife with Melody

Melody speaks MPD, so it connects like any other MPD server. Trackknife
controls its queue and outputs and automatically registers itself as a Melody
playback output when the server advertises `melody_version`. Stock MPD remains
an ordinary client connection.

## Configure melodyd

Install `melodyd` following the [Melody build instructions](https://github.com/carnager/melody-music#getting-started).
Install `mpv` on machines that will play audio.

Create `~/.config/melody/melodyd.toml` (or
`$XDG_CONFIG_HOME/melody/melodyd.toml` if you use a custom config directory):

```toml
[server]
name = "Music server"
bind_to_address = ["127.0.0.1:6701"]

[library]
music_dir = "/srv/music"

[player]
mpv_path = "mpv"

[mpd]
port = 6600
```

Replace `/srv/music` with your music folder. The user running `melodyd` needs
read access to it. If a config already exists, edit the matching sections
instead of adding duplicate headings.

Port `6600` is for MPD control. Port `6701` serves Melody's HTTP API, artwork,
and streams. `bind_to_address` controls the HTTP listener, not the MPD listener.
Melody's TCP MPD listener binds to all IPv4 interfaces and does not enforce
password authentication. Restrict it with a firewall to trusted machines;
do not expose it to the public internet. `server.web_secret` does not protect
the TCP MPD port.

Start the daemon in a terminal:

```sh
melodyd
```

For a source build, run `./bin/melodyd` from the Melody checkout instead.
The daemon scans the music folder on startup.

If your package installed the user service, use this instead of running it
manually:

```sh
systemctl --user enable --now melodyd
```

After config changes, restart it with `systemctl --user restart melodyd`.
Service logs are available with `journalctl --user -u melodyd -e`.

## Connect Trackknife

Open **File → Connect to MPD…**:

- **Host or socket:** `127.0.0.1`, or the Melody server's hostname/IP.
- **Port:** `6600`, matching `[mpd] port`.
- **Password:** leave blank for Melody.
- **Local music root:** leave blank unless you also want to edit server files.

Connect, select **MPD Queue**, and browse or search the server library. Use the
output controls to enable the server's speakers, **Trackknife**, or another
connected agent. Trackknife becomes offline when this connection or the app is
closed and reconnects with the same process identity after transient failures.

If **Local music root** contains the same relative paths as Melody's library,
Trackknife decodes those files directly. Otherwise it streams by Melody's
stable song identity from the server HTTP endpoint on port `6701`. The endpoint
uses the MPD queue's identity and clock in either mode; it does not transfer the
row into a Local Queue tab.

## Play on another machine

Install `melody-agent` and `mpv` on the playback machine. In its
`~/.config/melody/melody-agent.toml`, set:

```toml
[agent]
name = "Desktop"
master = "192.168.1.10:6600"
music_dir = ""
```

Replace the address with your server's. Leaving `music_dir` empty makes the
agent stream from Melody. On the server, update the existing `[server]`
section so the agent can reach those streams:

```toml
[server]
name = "Music server"
bind_to_address = ["192.168.1.10:6701"]
base_url = "http://192.168.1.10:6701"
```

Use the server's actual LAN address in both fields. Allow ports `6600` and
`6701` only from trusted machines; this example has no HTTP authentication.
Restart `melodyd`, run `melody-agent` on the playback machine, and enable
**Desktop** in Trackknife's MPD outputs. Disable other outputs if you only
want that machine to play.

## Edit files from the server library

Mount the server's music folder locally, for example through NFS or sshfs.
Set **Local music root** in Trackknife's connection dialog to that mount.
The relative paths must match: if Melody reports `Artist/Album/01.flac`, a
root of `/mnt/music` must contain `/mnt/music/Artist/Album/01.flac`.

You can then open mapped tracks in a local tab for tagging or conversion.
Writes need filesystem permissions; the MPD connection does not grant them.
The mapping is separate from the optional local library and does not add
folders to it.

For more server options, see [Melody's configuration reference](https://github.com/carnager/melody-music#configuration-reference).

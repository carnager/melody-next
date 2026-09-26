# Setup

## On one computer

1. Start Trackknife. It starts its own engine; nothing to configure.
2. In the library panel, click **Folders…** and add your music folder.
3. Click **Refresh**.

That's it. Close the window and the music keeps playing; open it again and
it's where you left it.

## Music on a server

On the server, install `melodyd` ([Installation](install.md)) and give it a
name and a password in `~/.config/melody/melodyd.conf`:

```sh
MELODYD_OPTIONS=--name myserver --password-file /home/you/.config/melody/password
```

The password file holds one line; make it readable only by you
(`chmod 600`). Without a password the engine doesn't listen on the network
at all. Use the same password on every engine and agent you run.

```sh
systemctl --user enable --now melodyd
loginctl enable-linger $USER    # keep it running when you're logged out
```

Or use the Docker image instead.

On your desktop, in Trackknife:

1. **Settings → Engine → Remote engine**: pick the server under
   **On the network** and enter its password.
2. The server's library shows up as its own tab in the library panel. Click
   **Folders…** there and add the music folder as the *server* sees it, then
   **Refresh**.

Tabs you fill from the server's library play on the server.

## Editing tags on server files

The engine never writes to your files; Trackknife does. Mount the server's
music on your desktop (NFS, sshfs, …), then under **Settings → Engine** set:

- **Remote music folder**: the path on the server, e.g. `/music`
- **Mounted here at**: where it is on your desktop, e.g. `/mnt/nas/music`

Now the tag editor, ReplayGain and Convert work on server tracks.

## Other speakers

- **A computer running Trackknife**: tick **Settings → Engine → Let other
  engines play on this computer's speakers**.
- **A headless machine** (a Pi by the stereo): run
  `melody-agent --password-file FILE`, or `melodyd --agent` with the same
  password if it should have its own library too.
- **Your phone**: the app offers the phone as a speaker by default.

Pick where it plays from the output menu in Trackknife's header.

## The phone

Install the APK ([Installation](install.md#android)) and open it. It finds
engines on your network; **Or by address** takes a host and port for when
you're connected over a VPN.

On mobile data it streams Opus (128 kbps by default) instead of the original
files. The download button on an album keeps it on the phone (Opus 160, over
Wi-Fi only, unless you change that), for when there's no engine in reach.

## Away from home

Use a VPN (WireGuard, Tailscale, …) and connect to the engine's VPN address.
Don't forward its ports to the internet: the password travels unencrypted.
If you want TLS anyway, put a proxy in front of it (see
[melody.md](melody.md#headless-melodyd)).

How all of this fits together: [Engines, agents and the phone](melody.md).

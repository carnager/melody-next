# Setup

Three ways to set it up, each a few steps. How it all fits together, and
everything these leave out: [Engines, agents and the phone](melody.md).

## On one computer

1. Install Melody and `melodyd` ([Installation](install.md)) and start
   Melody. The engine is a user service and starts on the first
   connection; there is nothing to configure.
2. In the library panel, click **Folders…** and add your music folder.
3. Click **Refresh**.

Close the window and the music keeps playing; open it again and it is where
you left it.

## The library on a server

**On the server**, install `melodyd` and give it a password -- one for all
your engines:

```sh
melody-cli config password 'the one all your engines share'
systemctl --user enable --now melodyd
loginctl enable-linger $USER    # keep it running when you're logged out
```

`melody-cli config --edit` opens the whole `~/.config/melody/melodyd.conf`,
every setting explained in it. Without a password the engine stays on its
machine. Or use the [Docker image](install.md#docker).

**In Melody**, under **Settings → Engines**:

1. Enter the same password at the top.
2. Pick the server from **On the network** (or **Add** it by address) and
   choose it as the one that **holds the library**.
3. Add its music folder as the server sees it -- the folder browser shows
   the server's disk -- then click **Refresh** in the library panel.

The library's tabs play on the server. The tagger, ReplayGain and Convert
work on its files there; nothing needs mounting.

The sidebar's **This computer** shows this computer's own folders. Tabs of
those files are marked with a computer and play here, on this computer's
engine.

If the music is on a NAS and the engine runs elsewhere, run `melody-watch` on
the NAS so the engine hears about changes instead of scanning over the
network ([how](melody.md#a-nas-that-cant-run-the-engine)).

## Speakers and the phone

Pick where it plays from the output menu in Melody's header.

- **Another computer with an engine** plays for the others by default, when
  it has the same password. In Melody that is **Settings → Engines → Let
  other engines play on this computer's speakers**.
- **A headless machine** (a Pi by the stereo): `melody-agent --password-file
  FILE`.
- **A UPnP receiver**: `upnp = yes` in the server's `melodyd.conf`
  ([more](melody.md#upnp-speakers)).
- **The phone**: install the APK ([Installation](install.md#android)). It
  finds engines on your network, or takes a host and port under **Or by
  address**, and offers itself as a speaker. On mobile data it streams Opus
  instead of the original files; the download button on an album keeps it
  on the phone.

**Away from home**, use a VPN (WireGuard, Tailscale, …) and connect to the
engine's VPN address. Don't forward its ports to the internet: the password
travels unencrypted. For TLS, put a proxy in front of it
([how](melody.md#headless-melodyd)).

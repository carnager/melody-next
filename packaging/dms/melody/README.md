# Melody for DankMaterialShell

A bar widget: what Melody plays, previous / play-pause / next, and a heart.
The heart gives the track five stars and loves it on Last.fm; click it again
to take both back. Last.fm love uses the engine's own account, the one it
scrobbles with.

It runs `melody-cli`, so that has to be installed.

```sh
ln -s "$PWD" ~/.config/DankMaterialShell/plugins/melody
```

Then enable it under **Settings → Plugins** and add it to the bar. In its
settings, **Engine** picks which engine to follow: a name on the network or
`HOST:PORT`. Left empty, it's the engine on this computer.

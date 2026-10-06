# Melody for DankMaterialShell

A bar widget: what Melody plays, previous / play-pause / next, and a heart.
The heart gives the track five stars and loves it on Last.fm; click it again
to take both back. Last.fm love uses the engine's own account, the one it
scrobbles with.

Click the text for what plays: its cover, title, artist, album and year, the
same controls, and five stars to rate it on the 0-10 scale every Melody
client uses -- the left half of a star gives the half, the rating already
set, clicked, clears it.

It runs `melody-cli`, so that has to be installed.

```sh
ln -s "$PWD" ~/.config/DankMaterialShell/plugins/melody
```

Then enable it under **Settings → Plugins** and add it to the bar. Left as
it is, it follows whichever engine is playing, on this computer or on the
network, and its buttons act on that one. **Engine** in its settings pins it
to one: a name on the network or `HOST:PORT`.

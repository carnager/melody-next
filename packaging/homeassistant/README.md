# Melody for Home Assistant

A melodyd engine as a Home Assistant media player: what plays, with its
cover, how far in and how loud; play, pause, next, previous, seek, volume,
shuffle and repeat; and which output it plays on -- an agent, a UPnP
renderer, a sound card -- as the player's source. Changes come as the engine
says them, not by polling.

## What the engine needs

- melodyd listening on the network, which needs a password:
  `melodyd --listen 0.0.0.0:6603 --password-file ~/.config/melody/password`
  (6603 is the default when a password is set).
- Protocol level 13 or newer for the track's title, artist, album and cover
  (`playback.now_playing`). An older engine still plays and is controlled,
  without them.

## Installing

Copy `custom_components/melody` into the `custom_components` folder of your
Home Assistant configuration and restart Home Assistant. The engine is then
found on the network by itself (it announces `_melody._tcp`) and only its
password is asked; or add it under Settings -> Devices & services -> Add
integration -> Melody, with its host, port and password.

## Tests

Against an engine faked over a real socket, and -- when `MELODYD` names a
binary -- a real melodyd, run local-only so nothing is announced:

    uv venv --python 3.14 .venv
    uv pip install --python .venv/bin/python pytest-homeassistant-custom-component
    MELODYD=../../build/dev/src/daemon/melodyd .venv/bin/python -m pytest

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# Runs a command with audio that goes nowhere: PipeWire with a null sink,
# and WirePlumber to link streams to it, in a D-Bus session of their own.
# The tests then play for real -- tracks advance, outputs report -- in a
# container with no sound card.
set -eu
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/ci-runtime}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
config="${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"
mkdir -p "$config"
cat > "$config/ci-null-sink.conf" <<'CONF'
context.objects = [
    { factory = adapter
      args = {
          factory.name     = support.null-audio-sink
          node.name        = "ci-null-sink"
          node.description = "CI null sink"
          media.class      = "Audio/Sink"
          audio.position   = [ FL FR ]
          node.driver      = true
          priority.driver  = 1000
          priority.session = 1000
          object.linger    = true
      }
    }
]
CONF
exec dbus-run-session -- sh -c '
    pipewire &
    wireplumber &
    # Until the sink is there to be found.
    for _ in $(seq 50); do
        pw-cli ls Node 2>/dev/null | grep -q ci-null-sink && break
        sleep 0.2
    done
    "$@"
' sh "$@"

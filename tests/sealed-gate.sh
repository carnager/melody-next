#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# The test suite sealed off from the session it runs in: a network of its
# own (only its loopback, multicast on it), a session bus of its own, and no
# audio server -- no test can reach the user's engines, desktop or speakers.
#   tests/sealed-gate.sh BUILD-DIR [ctest arguments...]
set -euo pipefail
build="$1"; shift
none="$(mktemp -d)"
trap 'rm -rf "${none}"' EXIT
exec unshare -rn dbus-run-session -- sh -c '
    ip link set lo up
    # A network of its own to find engines on: multicast DNS needs an
    # interface that is not loopback.
    ip link add sealed0 type dummy
    ip addr add 10.213.0.1/24 dev sealed0
    ip link set sealed0 multicast on up
    ip route add 224.0.0.0/4 dev sealed0
    export PIPEWIRE_RUNTIME_DIR="$1" PULSE_SERVER=unix:/nonexistent
    shift
    exec ctest --test-dir "$@"
' sealed "${none}" "${build}" "$@"

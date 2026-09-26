#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0234: an engine is the same engine after a restart -- the id it gives
# in engine.info is kept in its state directory.

set -euo pipefail

melodyd="$1"
work="$(mktemp -d)"
daemon_pid=""

cleanup() {
    if [ -n "${daemon_pid}" ] && kill -0 "${daemon_pid}" 2>/dev/null; then
        kill "${daemon_pid}" 2>/dev/null || true
        wait "${daemon_pid}" 2>/dev/null || true
    fi
    rm -rf "${work}"
}
trap cleanup EXIT

fail() {
    echo "FAILED: $1" >&2
    cat "${work}/engine.log" >&2 || true
    exit 1
}

socket="${work}/melodyd.sock"

# Starts the engine, asks who it is, stops it.
engine_id() {
    "${melodyd}" --socket "${socket}" --state "${work}/state" --local-only 2>"${work}/engine.log" &
    daemon_pid=$!
    for _ in $(seq 1 100); do
        [ -S "${socket}" ] && break
        sleep 0.05
    done
    [ -S "${socket}" ] || fail "the engine starts"
    python3 - "${socket}" <<'PY'
import json, socket, sys
connection = socket.socket(socket.AF_UNIX)
connection.settimeout(10)
connection.connect(sys.argv[1])
connection.sendall(b'{"id":1,"method":"engine.info"}\n')
buffer = b""
while True:
    buffer += connection.recv(65536)
    while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        message = json.loads(line)
        if message.get("id") == 1:
            print(message["result"]["id"])
            sys.exit(0)
PY
    kill "${daemon_pid}"
    wait "${daemon_pid}" 2>/dev/null || true
    daemon_pid=""
}

first="$(engine_id)"
[ -n "${first}" ] || fail "engine.info names the engine"
[ "$(engine_id)" = "${first}" ] || fail "a restarted engine is the same engine"
[ "$(tr -d '\n' < "${work}/state/engine-id")" = "${first}" ] \
    || fail "its id is kept in its state directory"

echo "not an id" > "${work}/state/engine-id"
replaced="$(engine_id)"
[ "${replaced}" != "not an id" ] && [ -n "${replaced}" ] || fail "a damaged id is replaced"
[ "$(engine_id)" = "${replaced}" ] || fail "and the replacement kept"

rm "${work}/state/engine-id"
[ "$(engine_id)" != "${replaced}" ] || fail "an engine without its id file is a new one"

echo "engine identity: ok"

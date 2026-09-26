#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0234: one engine plays for several by address -- --play-for given
# again -- and each lists it among its outputs.

set -euo pipefail

melodyd="$1"
work="$(mktemp -d)"
pids=()

cleanup() {
    for pid in "${pids[@]}"; do
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf "${work}"
}
trap cleanup EXIT

fail() {
    echo "FAILED: $1" >&2
    tail -n 20 "${work}"/*.log >&2 || true
    exit 1
}

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

password="play-for-test"
start() { # name port
    "${melodyd}" --socket "${work}/$1.sock" --state "${work}/$1" --name "$1" \
        --listen "127.0.0.1:$2" --password "${password}" 2>"${work}/$1.log" &
    pids+=($!)
}
port_a="$(free_port)"
port_b="$(free_port)"
start alpha "${port_a}"
start beta "${port_b}"
for _ in $(seq 1 100); do
    [ -S "${work}/alpha.sock" ] && [ -S "${work}/beta.sock" ] && break
    sleep 0.05
done

# The password before any --play-for is every one's; each name after its own.
"${melodyd}" --socket "${work}/gamma.sock" --state "${work}/gamma" --name gamma --local-only \
    --play-for-password "${password}" \
    --play-for "127.0.0.1:${port_a}" --play-for-name alpha \
    --play-for "127.0.0.1:${port_b}" --play-for-name beta 2>"${work}/gamma.log" &
pids+=($!)

# Without audio here there is nothing to play for anyone with: skipped.
for _ in $(seq 1 60); do
    if grep -q "no audio here" "${work}/gamma.log" 2>/dev/null; then
        echo "no audio output here: skipped"
        exit 77
    fi
    grep -q "playing for" "${work}/gamma.log" 2>/dev/null && break
    sleep 0.1
done

# Each engine lists gamma among its outputs, online.
listed() { # port
    python3 - "$1" "${password}" <<'PY'
import json, socket, sys, time
port, password = int(sys.argv[1]), sys.argv[2]
deadline = time.time() + 10
while time.time() < deadline:
    connection = socket.create_connection(("127.0.0.1", port), timeout=5)
    stream = connection.makefile("rw")
    def call(i, method, params):
        stream.write(json.dumps({"id": i, "method": method, "params": params}) + "\n")
        stream.flush()
        while True:
            message = json.loads(stream.readline())
            if message.get("id") == i:
                return message
    call(1, "session.authenticate", {"password": password})
    outputs = call(2, "outputs.list", {}).get("result", {}).get("outputs", [])
    connection.close()
    if any(o.get("id") == "agent:gamma" and o.get("online") for o in outputs):
        print("yes")
        sys.exit(0)
    time.sleep(0.2)
print("no")
PY
}
[ "$(listed "${port_a}")" = "yes" ] || fail "alpha lists gamma's speakers"
[ "$(listed "${port_b}")" = "yes" ] || fail "and so does beta"
[ "$(grep -c "playing for" "${work}/gamma.log")" -eq 2 ] || fail "gamma plays for both"
echo "engine play-for: ok"

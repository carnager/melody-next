#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0286: melodyd run by a service manager -- the socket handed over as
# systemd does -- and its melodyd.conf over the protocol: read, changed as
# melodyd reads it, and a restart asked for.

set -euo pipefail

melodyd="$1"
work="$(mktemp -d /tmp/melodyd-service.XXXXXX)"
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

command -v systemd-socket-activate >/dev/null || { echo "no systemd-socket-activate: skipped"; exit 77; }

cat >"${work}/melodyd.conf" <<'CONF'
# The test engine.
name = handed-over
password = kept secret
agent = no

# How it streams.
#agent-stream-away = opus 128
CONF
chmod 600 "${work}/melodyd.conf"

# As systemd does for melodyd.socket: the socket made and listened on first,
# melodyd started on the first connection with it as descriptor 3.
env -u INVOCATION_ID systemd-socket-activate -l "${work}/s.sock" -- \
    "${melodyd}" --config "${work}/melodyd.conf" --state "${work}/state" --local-only \
    2>"${work}/engine.log" &
pids+=($!)
for _ in $(seq 1 100); do
    [ -S "${work}/s.sock" ] && break
    sleep 0.05
done

ask() { # method params-json
    python3 - "${work}/s.sock" "$1" "$2" <<'PY'
import json, socket, sys, time
path, method, params = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
deadline = time.time() + 10
while True:
    try:
        connection = socket.socket(socket.AF_UNIX)
        connection.connect(path)
        break
    except OSError:
        if time.time() > deadline:
            raise
        time.sleep(0.1)
stream = connection.makefile("rw")
stream.write(json.dumps({"id": 1, "method": method, "params": params}) + "\n")
stream.flush()
while True:
    line = stream.readline()
    if not line:
        print(json.dumps({"closed": True}))
        break
    message = json.loads(line)
    if message.get("id") == 1:
        print(json.dumps(message))
        break
PY
}
field() { # json python-expression
    python3 -c "import json,sys; m=json.loads(sys.argv[1]); print($2)" "$1"
}

info="$(ask engine.info '{}')"
[ "$(field "${info}" 'm["result"]["name"]')" = handed-over ] || fail "the handed-over socket is served: ${info}"
[ "$(field "${info}" 'm["result"]["service"]')" = True ] || fail "and the engine says a service manager runs it"
[ "$(field "${info}" 'm["result"]["level"]')" -ge 14 ] || fail "at level 14"

got="$(ask config.get '{}')"
[ "$(field "${got}" 'm["result"]["path"]')" = "${work}/melodyd.conf" ] || fail "config.get names its file"
[ "$(field "${got}" 'm["result"]["settings"]["name"]')" = handed-over ] || fail "and its settings"
[ "$(field "${got}" '"password" in m["result"]["settings"]')" = False ] || fail "but never a password"
[ "$(field "${got}" 'm["result"]["secrets"]')" = "['password']" ] || fail "only that it is set"
! grep -q "kept secret" <<<"${got}" || fail "the password is nowhere in the answer"

set_answer="$(ask config.set '{"changes":[{"key":"agent-stream-away","value":"opus 96"},{"key":"address","engine":"gemenon","value":"192.168.0.200:6603"},{"key":"agent","value":null}]}')"
[ "$(field "${set_answer}" 'm["result"]["restart"]')" = True ] || fail "config.set writes: ${set_answer}"
grep -A1 -x "#agent-stream-away = opus 128" "${work}/melodyd.conf" | grep -qx "agent-stream-away = opus 96" ||
    fail "a setting lands below its example"
grep -A1 -x "\[engine gemenon\]" "${work}/melodyd.conf" | grep -qx "address = 192.168.0.200:6603" ||
    fail "an [engine] section is made"
! grep -q "^agent = " "${work}/melodyd.conf" || fail "a null value takes a key out"
grep -qx "# The test engine." "${work}/melodyd.conf" || fail "comments are kept"
[ "$(stat -c %a "${work}/melodyd.conf")" = 600 ] || fail "and so are its permissions"

cp "${work}/melodyd.conf" "${work}/before"
bad="$(ask config.set '{"changes":[{"key":"agent-stream-home","value":"opus 96"},{"key":"agent-stream-away","value":"flac"}]}')"
grep -q "agent-stream-away is original, or opus" <<<"${bad}" || fail "a value melodyd would refuse is refused: ${bad}"
cmp -s "${work}/before" "${work}/melodyd.conf" || fail "all or none: the file is as it was"

restart="$(ask engine.restart '{}')"
[ "$(field "${restart}" 'm["result"]["restarting"]')" = True ] || fail "engine.restart answers"
for _ in $(seq 1 100); do
    kill -0 "${pids[0]}" 2>/dev/null || break
    sleep 0.1
done
! kill -0 "${pids[0]}" 2>/dev/null || fail "and the engine stops for its service manager"
# The socket is the service manager's: left where it is.
[ -S "${work}/s.sock" ] || fail "a handed-over socket's file is left in place"

# Started without one, it says so.
env -u INVOCATION_ID "${melodyd}" --config "${work}/melodyd.conf" --state "${work}/state" \
    --socket "${work}/own.sock" --local-only 2>"${work}/own.log" &
pids+=($!)
for _ in $(seq 1 100); do
    [ -S "${work}/own.sock" ] && break
    sleep 0.05
done
own="$(python3 - "${work}/own.sock" <<'PY'
import json, socket, sys
connection = socket.socket(socket.AF_UNIX)
connection.connect(sys.argv[1])
stream = connection.makefile("rw")
stream.write(json.dumps({"id": 1, "method": "engine.info", "params": {}}) + "\n")
stream.flush()
while True:
    message = json.loads(stream.readline())
    if message.get("id") == 1:
        print(message["result"]["service"])
        break
PY
)"
[ "${own}" = False ] || fail "an engine started by hand is not a service's"
echo "engine service: ok"

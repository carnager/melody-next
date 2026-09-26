#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0232: melody-watch against a real engine over TCP with a password, as
# on a NAS: what changed while it was not running is caught up, and what
# changes while it runs reaches the library -- files, a folder moved in, a
# folder moved out, a file deleted -- with no scan.

set -euo pipefail

melodyd="$1"
watcher="$2"
work="$(mktemp -d)"
daemon_pid=""
watch_pid=""

cleanup() {
    for pid in "${watch_pid}" "${daemon_pid}"; do
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    rm -rf "${work}"
}
trap cleanup EXIT

fail() {
    echo "FAILED: $1" >&2
    echo "--- engine log" >&2
    cat "${work}/engine.log" >&2 || true
    echo "--- watcher log" >&2
    cat "${work}/watch.log" >&2 || true
    exit 1
}

wave() {
    mkdir -p "$(dirname "$1")"
    python3 - "$1" <<'PY'
import sys, wave
with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(44100)
    out.writeframes(b"\0" * 44100 * 4)
PY
}

wave "${work}/music/First/one.wav"
socket="${work}/melodyd.sock"
port=$(( 20000 + RANDOM % 20000 ))
printf 'correct horse\n' > "${work}/password"
"${melodyd}" --socket "${socket}" --state "${work}/state" --listen "127.0.0.1:${port}" \
    --password-file "${work}/password" 2>"${work}/engine.log" &
daemon_pid=$!
for _ in $(seq 1 100); do
    [ -S "${socket}" ] && grep -q "with a password" "${work}/engine.log" 2>/dev/null && break
    sleep 0.05
done
[ -S "${socket}" ] || fail "the engine listens"

# One request over the engine's own socket, answered or waited for until an
# event: the test's view of the library, not the watcher's.
engine() {
    python3 - "${socket}" "$1" "${2:-}" <<'PY'
import json, socket, sys
path, request, until = sys.argv[1], sys.argv[2], sys.argv[3]
connection = socket.socket(socket.AF_UNIX)
connection.settimeout(30)
connection.connect(path)
connection.sendall(request.encode() + b"\n")
wanted = json.loads(request)["id"]
buffer = b""
while True:
    chunk = connection.recv(65536)
    if not chunk:
        break
    buffer += chunk
    while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        message = json.loads(line)
        if (until and message.get("event") == until) or (not until and message.get("id") == wanted):
            print(line.decode())
            sys.exit(0)
PY
}
tracks() {
    engine '{"id":1,"method":"catalogue.paths","params":{"kind":2,"limit":1000}}' |
        python3 -c 'import base64,json,sys; print("\n".join(sorted(base64.b64decode(p).decode() for p in json.load(sys.stdin)["result"]["paths"])))'
}
# Waits for the library to hold exactly these files, relative to music/;
# the first argument says what that shows.
expect() {
    local what="$1" wanted
    shift
    wanted="$(printf '%s\n' "$@" | sed "s|^|${work}/music/|" | sort)"
    for _ in $(seq 1 100); do
        [ "$(tracks)" = "${wanted}" ] && return 0
        sleep 0.1
    done
    echo "library holds:" >&2
    tracks >&2
    fail "${what}"
}

encoded="$(python3 -c 'import base64,sys; print(base64.b64encode(sys.argv[1].encode()).decode())' "${work}/music")"
engine "{\"id\":1,\"method\":\"catalogue.add_root\",\"params\":{\"path\":\"${encoded}\"}}" > /dev/null
engine '{"id":2,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished > /dev/null
[ "$(tracks)" = "${work}/music/First/one.wav" ] || fail "the library is scanned once"

# Changed while nothing watched.
wave "${work}/music/Second/two.wav"

"${watcher}" --server "127.0.0.1:${port}" --password-file "${work}/password" --settle 0 \
    "${work}/music" 2>"${work}/watch.log" &
watch_pid=$!
expect "the watcher catches up with what changed before it ran" "First/one.wav" "Second/two.wav"
grep -q "1 new, 0 changed, 0 gone" "${work}/watch.log" || fail "and says what it found"

# A folder made and filled while it runs.
wave "${work}/music/Third/CD1/three.wav"
expect "a new folder's file reaches the library" "First/one.wav" "Second/two.wav" "Third/CD1/three.wav"

# Moved out whole, then a file deleted.
mv "${work}/music/Third" "${work}/elsewhere"
expect "a folder moved out leaves the library" "First/one.wav" "Second/two.wav"
rm "${work}/music/First/one.wav"
expect "a deleted file leaves the library" "Second/two.wav"

# Moved back in, under a name of its own.
mv "${work}/elsewhere" "${work}/music/Back"
expect "a folder moved in is read" "Back/CD1/three.wav" "Second/two.wav"

# And then it is quiet. The engine reading a file used to open it for
# writing, which inotify reports as a change when it is closed: the watcher
# sent it again, the engine read it again, and round it went.
sent="$(grep -c "sent " "${work}/watch.log" || true)"
sleep 2
[ "$(grep -c "sent " "${work}/watch.log" || true)" = "${sent}" ] ||
    fail "the engine reading a file is not taken for the file changing"

# A wrong password is said, and the watcher keeps what it has for later.
kill "${watch_pid}"; wait "${watch_pid}" 2>/dev/null || true
"${watcher}" --server "127.0.0.1:${port}" --password "wrong" --settle 0 "${work}/music" \
    2>"${work}/watch.log" &
watch_pid=$!
for _ in $(seq 1 50); do
    grep -q "wrong password" "${work}/watch.log" 2>/dev/null && break
    sleep 0.1
done
grep -q "wrong password" "${work}/watch.log" || fail "a refused password is said"
kill -0 "${watch_pid}" || fail "and the watcher keeps running"

echo "melody-watch: ok"

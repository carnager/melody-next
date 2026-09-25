#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# melody-cli against a real engine, as a script or a key binding uses it.

set -euo pipefail

melodyd="$1"
cli="$2"
work="$(mktemp -d)"
daemon_pid=""
second_pid=""

cleanup() {
    if [ -n "${second_pid}" ] && kill -0 "${second_pid}" 2>/dev/null; then
        kill "${second_pid}" 2>/dev/null || true
        wait "${second_pid}" 2>/dev/null || true
    fi
    if [ -n "${daemon_pid}" ] && kill -0 "${daemon_pid}" 2>/dev/null; then
        kill "${daemon_pid}" 2>/dev/null || true
        wait "${daemon_pid}" 2>/dev/null || true
    fi
    rm -rf "${work}"
}
trap cleanup EXIT

fail() {
    echo "FAILED: $1" >&2
    echo "--- engine log" >&2
    cat "${work}/engine.log" >&2 || true
    exit 1
}

# Silence, so a test that plays makes no sound: two short albums' worth.
mkdir -p "${work}/music/first" "${work}/music/second"
python3 - "${work}/music" <<'PY'
import sys, wave
root = sys.argv[1]
for path in ("first/alpha.wav", "first/beta.wav", "second/gamma.wav"):
    with wave.open(f"{root}/{path}", "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(44100)
        out.writeframes(b"\0" * 44100 * 4 * 30)
PY

socket="${work}/melodyd.sock"
"${melodyd}" --socket "${socket}" --state "${work}/state" --local-only 2>"${work}/engine.log" &
daemon_pid=$!
for _ in $(seq 1 100); do
    [ -S "${socket}" ] && break
    sleep 0.05
done
[ -S "${socket}" ] || fail "the engine starts"

# One request, read until the line that answers it -- or, for a job, the line
# that says it finished: the engine keeps the connection open, as it must.
engine() {
    python3 - "${ENGINE_SOCKET:-${socket}}" "$1" "${2:-}" <<'PY'
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
        if until and message.get("event") == until:
            print(line.decode())
            sys.exit(0)
        if not until and message.get("id") == wanted:
            print(line.decode())
            sys.exit(0)
PY
}
encoded="$(python3 -c 'import base64,sys; print(base64.b64encode(sys.argv[1].encode()).decode())' "${work}/music")"
engine "{\"id\":1,\"method\":\"catalogue.add_root\",\"params\":{\"path\":\"${encoded}\"}}" > /dev/null
engine '{"id":2,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished | grep -q job.finished \
    || fail "the library is scanned"

cli() { "${cli}" --server "${socket}" "$@"; }

cli --help | grep -q "play  album|track WORDS" || fail "--help says how to play an album"

# The library, by words and by what is newest.
cli tracks alpha | grep -q "alpha" || fail "a track is found by its title"
[ "$(cli latest 5 | wc -l)" -ge 1 ] || fail "the newest albums are listed"
cli --json albums | python3 -c 'import json,sys; assert len(json.load(sys.stdin)) == 2' \
    || fail "--json answers in JSON, every album"

# Quiet first, then play a track: the queue is replaced and it plays.
cli volume 0 > /dev/null
[ "$(cli volume)" = "0" ] || fail "the volume is set and read back"
cli play track alpha 2>/dev/null | grep -q "^playing: .*alpha" || fail "play track plays it"
cli status | grep -q "queue 1" || fail "status shows the queue it replaced"
cli status | grep -q " / ?" && fail "the library's duration travels with what is queued"

# Pause and toggle.
cli pause | grep -q "^paused:" || fail "pause pauses"
cli toggle | grep -q "^playing:" || fail "toggle resumes"

# Added after it, the queue grows and what plays keeps playing.
cli add track gamma 2>/dev/null > /dev/null
cli status | grep -q "^playing: .*alpha" || fail "adding leaves what plays alone"
cli status | grep -q "queue 2" || fail "and appends"

# Up Next, first and last.
cli queue track beta 2>/dev/null > /dev/null
cli next track gamma 2>/dev/null > /dev/null
requests="$(engine '{"id":3,"method":"playback.requests"}')"
[ "$(echo "${requests}" | python3 -c 'import json,sys; print(len(json.loads(sys.stdin.read())["result"]["entries"]))')" = "2" ] \
    || fail "two are asked for in Up Next"
cli status | grep -q "up next 2" || fail "status counts them"

# Skipping plays what was asked first: gamma.
cli next | grep -q "^playing: .*gamma" || fail "next plays what Up Next has first"

cli seek 10 > /dev/null
position="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["position_ms"])')"
[ "${position}" -ge 9000 ] || fail "seek moves the position (at ${position})"

# Stars for what plays: stored as the engine counts, 0-10.
cli rate 5 | grep -q "^rated 5 stars: .*gamma" || fail "rate rates what plays"
[ "$(cli rate)" = "5" ] || fail "rate reads the stars back"
cli --json rate | python3 -c 'import json,sys; assert json.load(sys.stdin)["rating"] == 10' \
    || fail "five stars are a 10 to the engine"
if cli rate 7 2>/dev/null; then
    fail "more than five stars fails"
fi

# No Last.fm account here: love says why and fails.
if cli love 2>"${work}/love.txt"; then
    fail "love without a Last.fm account fails"
fi
grep -q "lastfm.love" "${work}/love.txt" || fail "and says why"

# watch: a line as it starts, and one for each change.
"${cli}" --server "${socket}" --json watch > "${work}/watch.txt" &
watch_pid=$!
for _ in $(seq 1 100); do
    [ -s "${work}/watch.txt" ] && break
    sleep 0.05
done
cli pause > /dev/null
for _ in $(seq 1 100); do
    grep -q '"status":"paused"' "${work}/watch.txt" && break
    sleep 0.05
done
kill "${watch_pid}" 2>/dev/null || true
wait "${watch_pid}" 2>/dev/null || true
head -1 "${work}/watch.txt" | python3 -c '
import json, sys
state = json.load(sys.stdin)
assert state["status"] == "playing", state["status"]
assert "gamma" in state["track"]["title"], state["track"]
assert state["track"]["rating"] == 10, state["track"]' || fail "watch starts with what plays, and its rating"
grep -q '"status":"paused"' "${work}/watch.txt" || fail "watch prints a change"

# A rating set by another client reaches watch, without a playback change.
"${cli}" --server "${socket}" --json watch > "${work}/rated.txt" &
watch_pid=$!
for _ in $(seq 1 100); do
    [ -s "${work}/rated.txt" ] && break
    sleep 0.05
done
cli rate 2 > /dev/null
for _ in $(seq 1 100); do
    grep -q '"rating":4' "${work}/rated.txt" && break
    sleep 0.05
done
kill "${watch_pid}" 2>/dev/null || true
wait "${watch_pid}" 2>/dev/null || true
grep -q '"rating":4' "${work}/rated.txt" || fail "watch hears of a rating set elsewhere"
cli toggle > /dev/null

# watch --all: two engines, and the line is about the one that plays --
# the latest to start. Named with --server, so no other engine nearby is
# asked.
second="${work}/second.sock"
"${melodyd}" --socket "${second}" --state "${work}/second-state" --local-only \
    2>"${work}/second.log" &
second_pid=$!
for _ in $(seq 1 100); do
    [ -S "${second}" ] && break
    sleep 0.05
done
[ -S "${second}" ] || fail "a second engine starts"
ENGINE_SOCKET="${second}" engine \
    "{\"id\":1,\"method\":\"catalogue.add_root\",\"params\":{\"path\":\"${encoded}\"}}" > /dev/null
ENGINE_SOCKET="${second}" engine '{"id":2,"method":"job.submit","params":{"job":"catalogue.scan"}}' \
    job.finished | grep -q job.finished || fail "the second library is scanned"
cli2() { "${cli}" --server "${second}" "$@"; }
cli2 volume 0 > /dev/null
"${cli}" --server "${socket}" --server "${second}" --json watch --all > "${work}/all.txt" &
watch_pid=$!
for _ in $(seq 1 100); do
    [ -s "${work}/all.txt" ] && break
    sleep 0.05
done
last_all() { tail -1 "${work}/all.txt" | python3 -c '
import json, sys
state = json.load(sys.stdin)
print(state["engine"]["server"], state["status"], (state.get("track") or {}).get("title", ""))'; }
last_all | grep -q "^${socket} playing .*gamma" || fail "watch --all starts on the engine that plays"
cli2 play track alpha 2>/dev/null > /dev/null
for _ in $(seq 1 100); do
    last_all | grep -q "^${second} playing .*alpha" && break
    sleep 0.05
done
last_all | grep -q "^${second} playing .*alpha" || fail "it follows the engine that started last"
cli2 stop > /dev/null
for _ in $(seq 1 100); do
    last_all | grep -q "^${socket} playing" && break
    sleep 0.05
done
last_all | grep -q "^${socket} playing .*gamma" || fail "and back to the one still playing"
kill "${watch_pid}" 2>/dev/null || true
wait "${watch_pid}" 2>/dev/null || true
kill "${second_pid}" 2>/dev/null || true
wait "${second_pid}" 2>/dev/null || true
second_pid=""

cli outputs | grep -q "^\*" || fail "outputs marks the one in use"
cli stop | grep -q "^stopped:" || fail "stop stops"

# Only the fields asked for, and the key; every track without words.
cli --fields title --json tracks | python3 -c '
import json, sys
tracks = json.load(sys.stdin)
assert len(tracks) == 3, len(tracks)
assert all(set(track) == {"key", "title"} for track in tracks), tracks[0]' \
    || fail "--fields lists only those, with the key, and tracks lists every track"
# --keys: lines as a picker shows them, each ending in a tab and its key.
cli --keys tracks | python3 -c '
import sys
lines = sys.stdin.read().splitlines()
assert len(lines) == 3, lines
assert all(line.count("\t") == 1 and line.split("\t")[1] for line in lines), lines' \
    || fail "--keys ends every line in a tab and its key"
track_key="$(cli --fields title --json tracks gamma | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["key"])')"
before="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')"
cli add track --key "${track_key}" 2>/dev/null > /dev/null || fail "add track --key adds it"
after="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')"
[ "${after}" = "$((before + 1))" ] || fail "the keyed track is appended (${before} -> ${after})"

# One album exactly, by the key albums gives it -- as a picker does.
key="$(cli --json albums | python3 -c '
import json, sys
print(next(album["key"] for album in json.load(sys.stdin) if album["tracks"] == 1))')"
before="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')"
cli add album --key "${key}" 2>/dev/null > /dev/null || fail "add album --key adds it"
after="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')"
[ "${after}" = "$((before + 1))" ] || fail "the keyed album's one track is appended (${before} -> ${after})"
if cli add album --key nothing-like-this 2>/dev/null; then
    fail "a key no album has fails"
fi

# A queue entry with nothing but its path -- queued by a client that had
# not read the file yet: what plays is still told by the library's tags,
# not by its file name.
bare="$(python3 -c 'import base64,sys; print(base64.b64encode(sys.argv[1].encode()).decode())' \
    "${work}/music/first/beta.wav")"
entry="$(python3 -c 'import uuid; print(uuid.uuid4())')"
engine "{\"id\":1,\"method\":\"playback.replace_queue\",\"params\":{\"entries\":[{\"entry\":\"${entry}\",\"path\":\"${bare}\"}]}}" > /dev/null
engine "{\"id\":2,\"method\":\"playback.play\",\"params\":{\"entry\":\"${entry}\"}}" > /dev/null
library_says="$(cli tracks beta | head -1)"
cli status | head -1 | grep -qF "${library_says}" \
    || fail "a bare entry is shown as the library knows it (${library_says}), not by its name"
cli stop > /dev/null

# Wrong words say so, and fail.
if cli play album nothing-like-this 2>"${work}/none.txt"; then
    fail "an album that is not there fails"
fi
grep -q "no album matches" "${work}/none.txt" || fail "and says why"

echo "melody-cli: ok"

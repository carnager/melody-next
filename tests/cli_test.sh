#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# melody-cli against a real engine, as a script or a key binding uses it.

set -euo pipefail

# grep -q stops reading at the first match, and under pipefail the writer
# then dying of SIGPIPE fails the pipe: a slow writer (a sanitizer build)
# made that a race. This reads everything.
matches() { grep "$@" >/dev/null; }

melodyd="$1"
cli="$2"
work="$(mktemp -d)"
# The engines here are named on each line, not by whoever runs the test.
unset MELODY_SERVER MELODY_PASSWORD MELODY_PASSWORD_FILE
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
import os, time
for path in ("first/alpha.wav", "first/beta.wav", "second/gamma.wav"):
    with wave.open(f"{root}/{path}", "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(44100)
        out.writeframes(b"\0" * 44100 * 4 * 30)
    # Made long ago: a library's first scan dates its tracks by their files,
    # and these are not new to it.
    old = time.time() - 30 * 86400
    os.utime(f"{root}/{path}", (old, old))
PY

socket="${work}/melodyd.sock"
# A new album is said once no track of it has come for a second, not thirty.
MELODY_ALBUM_SETTLE_SECONDS=1 "${melodyd}" --socket "${socket}" --state "${work}/state" \
    --local-only 2>"${work}/engine.log" &
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
engine '{"id":2,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished | matches job.finished \
    || fail "the library is scanned"

cli() { "${cli}" --server "${socket}" "$@"; }

cli --help | matches "upnext  album|track WORDS" || fail "--help says how to play an album"
cli --help | matches "^Which engine" || fail "--help says how the engine is chosen"
cli help queue | matches "melody-cli play random tracks 50" \
    || fail "help COMMAND tells one command in full, with examples"
cli play --help | matches "melody-cli play list" || fail "COMMAND --help is help COMMAND"
if cli nosuch 2> "${work}/nosuch.txt"; then
    fail "an unknown command fails"
fi
matches "melody-cli help lists them" "${work}/nosuch.txt" || fail "and says where the commands are"
# Which engine answered, beside where it plays.
# ADR-0342: every command completion offers has its help.
"${cli}" __complete commands | cut -f1 > "${work}/commands.txt"
matches -x upnext "${work}/commands.txt" || fail "__complete commands lists them"
while read -r command; do
    "${cli}" help "${command}" > /dev/null || fail "help ${command}"
done < "${work}/commands.txt"
# config is this computer's file: an engine named on the line is refused.
if cli config name 2>"${work}/config.txt"; then
    fail "config with --server is refused"
fi
matches "this computer's melodyd.conf" "${work}/config.txt" || fail "and says why"
cli status | matches " · engine " || fail "status names the engine that answered"
# --local: this computer's engine, whatever $MELODY_SERVER says.
XDG_RUNTIME_DIR="${work}" MELODY_SERVER=192.0.2.1:9 "${cli}" --local status | matches " · engine " \
    || fail "--local reaches this computer's engine, past \$MELODY_SERVER"
if XDG_RUNTIME_DIR="${work}/nowhere" "${cli}" --local status 2> "${work}/local.txt"; then
    fail "--local with no engine here fails"
fi
matches "not running" "${work}/local.txt" || fail "and says so"
if "${cli}" --local --server "${socket}" status > /dev/null 2>&1; then
    fail "--local and --server together are two engines"
fi

# The library's revision: the same after a scan that finds nothing new,
# another once a track comes.
first_revision="$(cli revision)"
[ -n "${first_revision}" ] || fail "revision says one"
engine '{"id":7,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished > /dev/null
[ "$(cli revision)" = "${first_revision}" ] || fail "a scan finding nothing new leaves the revision"
python3 - "${work}/music/second/delta.wav" <<'PY'
import sys, wave
with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(44100)
    out.writeframes(b"\0" * 44100 * 4)
PY
# update: of one folder, given relative to here, then of everything.
(cd "${work}/music" && cli update second/) | matches "1 read" || fail "update reads the new file"
cli find delta --format '%path%' | matches -x -F "${work}/music/second/delta.wav" \
    || fail "update of a folder takes in what came to it"
second_revision="$(cli revision)"
[ "${second_revision}" != "${first_revision}" ] || fail "a track that came changes the revision"
rm "${work}/music/second/delta.wav"
cli update > /dev/null || fail "update of the whole library"
[ "$(cli revision)" != "${second_revision}" ] || fail "and one that went"
if cli update "${work}/elsewhere" > /dev/null 2>"${work}/update.txt"; then
    fail "update of a path in no library folder fails"
fi
matches -F "not in a library folder: ${work}/elsewhere" "${work}/update.txt" || fail "and says so"

# The library, by words and by what is newest.
cli tracks alpha | matches "alpha" || fail "a track is found by its title"

# A query, formatted by the engine with the library's whole row.
[ "$(cli find alpha --format '%title%|$info(samplerate)|$info(bitspersample)')" = "alpha|44100|16" ] \
    || fail "find formats what a query finds, technicals included"
[ "$(cli find alpha --format '$info(Sample-Rate)/$info(bits_per_sample)')" = "44100/16" ] \
    || fail "the engine looks names up folded: case, spaces, underscores, hyphens"
[ "$(cli find 'ALL SORT DESCENDING BY %title%' --format '%title%' | tr '\n' ' ')" = "gamma beta alpha " ] \
    || fail "find keeps the query's order"
cli find --keys alpha | matches -P "^.+\t[A-Za-z0-9+/=]+$" || fail "find --keys gives each line its key"
if cli find 'title IS' 2>"${work}/query.txt"; then
    fail "a query that does not parse is refused"
fi
[ -s "${work}/query.txt" ] || fail "and says why"
if cli find nothing-like-this > /dev/null 2>&1; then
    fail "find with nothing found fails, as tracks does"
fi
[ "$(cli latest 5 | wc -l)" -ge 1 ] || fail "the newest albums are listed"
# What came in since a moment, as a cron job asks it: names, or folders.
[ "$(cli latest --since 60d | wc -l)" -ge 1 ] || fail "latest --since lists what came in"
cli latest --since 60d --folders > "${work}/folders.txt" || fail "latest --since --folders answers"
while IFS= read -r folder; do
    case "${folder}" in
        "${work}/music/"*) [ -d "${folder}" ] || fail "an album's folder is a folder: ${folder}" ;;
        *) fail "an album's folder is in the music folder: ${folder}" ;;
    esac
done < "${work}/folders.txt"
[ -s "${work}/folders.txt" ] || fail "and there is one"
cli --json latest --since 60d --folders | python3 -c '
import json, sys
albums = json.load(sys.stdin)
assert albums and all(a["folder"] for a in albums)' || fail "--json gives each album its folder"
[ -z "$(cli latest --since "$(( $(date +%s) + 3600 ))")" ] || fail "nothing comes in from the future"
cli latest --since "$(( $(date +%s) + 3600 ))" || fail "and nothing new is no failure"
if cli latest --since whenever > /dev/null 2>&1; then
    fail "a moment latest cannot read is refused"
fi

# Converted here with Melody's converter: what came in, kept below the
# music folder; again, nothing done twice.
cli convert --presets | matches "^flac" || fail "convert lists its presets"
cli convert --to "${work}/converted" --preset flac --since 60d --no-replaygain --dry-run \
    > "${work}/plan.txt" \
    || fail "convert --dry-run plans"
[ "$(wc -l < "${work}/plan.txt")" -ge 1 ] || fail "and plans something"
matches -P "^${work}/music/.+\t${work}/converted/.+\.flac$" "${work}/plan.txt" \
    || fail "each line the source and its target, below the destination"
[ ! -d "${work}/converted" ] || [ -z "$(find "${work}/converted" -type f)" ] \
    || fail "a dry run converts nothing"
cli convert --to "${work}/converted" --preset flac --since 60d --no-replaygain \
    > "${work}/made.txt" \
    || fail "convert converts"
[ "$(wc -l < "${work}/made.txt")" = "$(wc -l < "${work}/plan.txt")" ] \
    || fail "every planned track converted"
while IFS= read -r made; do
    [ -s "${made}" ] || fail "a converted file is there: ${made}"
done < "${work}/made.txt"
cli convert --to "${work}/converted" --preset flac --since 60d --no-replaygain \
    > "${work}/again.txt" 2>&1 \
    || fail "converting again succeeds"
matches "converted before, passed over" "${work}/again.txt" || fail "and passes over what is there"
if cli convert --to "${work}/converted" --layout "No such layout" --since 60d > /dev/null 2>&1; then
    fail "a layout the engine does not have is refused"
fi
if cli convert --since 60d > /dev/null 2>&1; then
    fail "convert wants a destination"
fi
# ReplayGain measured afresh on what was made, by the engine: a tone long
# enough to measure, in the music folder, converted by its path.
mkdir -p "${work}/music/zz-gain"
python3 - "${work}/music/zz-gain/tone.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(44100)
    frames = bytearray()
    for n in range(44100 * 3):
        sample = int(8000 * math.sin(2 * math.pi * 440 * n / 44100))
        frames += struct.pack("<hh", sample, sample)
    out.writeframes(bytes(frames))
PY
cli convert --to "${work}/gained" --preset flac "${work}/music/zz-gain" > "${work}/gain.txt" \
    2> "${work}/gain-said.txt" || { cat "${work}/gain-said.txt" >&2; fail "convert measures ReplayGain"; }
matches "ReplayGain measured and written for 1 files" "${work}/gain-said.txt" \
    || fail "and says so"
python3 - "$(cat "${work}/gain.txt")" <<'PY' || fail "the converted file has its gain"
import sys
data = open(sys.argv[1], "rb").read().upper()
assert b"REPLAYGAIN_TRACK_GAIN=" in data, "no REPLAYGAIN_TRACK_GAIN"
PY
rm -rf "${work}/music/zz-gain"

# A watcher: each album new to the library, once whole, its command run with
# the folder -- and an album that only grew is not new.
cli watch albums --run 'printf "%s|%s\n" {} "${MELODY_ALBUM_TITLE}" >> '"${work}/arrived.txt" &
watcher=$!
sleep 1
mkdir -p "${work}/music/third"
python3 - "${work}/music" <<'PY'
import sys, wave
for path in ("third/delta.wav", "third/epsilon.wav", "first/zeta.wav"):
    with wave.open(f"{sys.argv[1]}/{path}", "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(44100)
        out.writeframes(b"\0" * 44100 * 4)
PY
engine '{"id":30,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished > /dev/null
for _ in $(seq 1 100); do
    [ -s "${work}/arrived.txt" ] && break
    sleep 0.1
done
sleep 2
kill "${watcher}" 2>/dev/null
wait "${watcher}" 2>/dev/null || true
[ "$(grep -c -F "${work}/music/third|" "${work}/arrived.txt" 2>/dev/null)" = "1" ] \
    || { cat "${work}/arrived.txt" >&2; fail "the new album, said once, with its folder"; }
if matches -F "${work}/music/first|" "${work}/arrived.txt"; then
    fail "an album that only grew is not new"
fi
matches -F "${work}/music/third|Unknown album" "${work}/arrived.txt" || fail "its title set"
rm -rf "${work}/music/third" "${work}/music/first/zeta.wav"
engine '{"id":31,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished > /dev/null
cli --json albums | python3 -c 'import json,sys; assert len(json.load(sys.stdin)) == 2' \
    || fail "--json answers in JSON, every album"

# Quiet first, then play a track: the queue is replaced and it plays.
cli volume 0 > /dev/null
[ "$(cli volume)" = "0" ] || fail "the volume is set and read back"
[ -z "$(cli current)" ] || fail "current prints nothing while nothing plays"
cli play track alpha 2>/dev/null | matches "^playing: .*alpha" || fail "play track plays it"
cli current | matches "alpha" || fail "current names the playing track"
cli current --format '%playback_state%|%title%|$if(%rating%,rated,unrated)|%length%' \
    | matches -E "^playing\|[^|]*alpha[^|]*\|unrated\|0:[0-9]{2}$" \
    || fail "current formats the track and where playback is, in tkfmt-1"
[ "$(cli current --format '$info(samplerate)/$info(bitspersample)/$info(channels)')" = "44100/16/2" ] \
    || fail "current has the library's technicals, through \$info"
[ "$(cli --format '$upper(x)\(%playback_state%\)' current)" = "X(playing)" ] \
    || fail "--format comes before the command too, with escaped parentheses"
if cli current --format '$nosuch(%title%)' 2>"${work}/format.txt"; then
    fail "a format that does not compile is refused"
fi
grep -q "unknown format function" "${work}/format.txt" || fail "and says why"
cli status | matches "queue 1" || fail "status shows the queue it replaced"
cli status | matches " / ?" && fail "the library's duration travels with what is queued"

# Pause and toggle.
cli pause | matches "^paused:" || fail "pause pauses"
cli toggle | matches "^playing:" || fail "toggle resumes"

# Added after it, the queue grows and what plays keeps playing.
cli add track gamma 2>/dev/null > /dev/null
cli status | matches "^playing: .*alpha" || fail "adding leaves what plays alone"
cli status | matches "queue 2" || fail "and appends"

# Up Next, first and last.
cli upnext track beta 2>/dev/null > /dev/null
cli insert track gamma 2>/dev/null > /dev/null
requests="$(engine '{"id":3,"method":"playback.requests"}')"
[ "$(echo "${requests}" | python3 -c 'import json,sys; print(len(json.loads(sys.stdin.read())["result"]["entries"]))')" = "2" ] \
    || fail "two are asked for in Up Next"
cli status | matches "up next 2" || fail "status counts them"

# next only skips: with words it says what does that.
if cli next track beta 2>"${work}/next.txt"; then
    fail "next with words is refused"
fi
matches "melody-cli insert" "${work}/next.txt" || fail "and points to insert"

# Skipping plays what was asked first: gamma.
cli next | matches "^playing: .*gamma" || fail "next plays what Up Next has first"

cli seek 10 > /dev/null
position="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["position_ms"])')"
[ "${position}" -ge 9000 ] || fail "seek moves the position (at ${position})"

# Ratings for what plays, 1-10 as the engine keeps them; 0 clears.
[ "$(cli rating)" = "0" ] || fail "an unrated track says 0"
cli rating 7 | matches "^rated 7: .*gamma" || fail "rating rates what plays"
[ "$(cli rating)" = "7" ] || fail "rating reads it back"
[ "$(cli current --format '%rating%')" = "7" ] || fail "current's rating is the same 1-10"
cli rating 10 > /dev/null || fail "rating rates again"
cli --json rating | python3 -c 'import json,sys; assert json.load(sys.stdin)["rating"] == 10' \
    || fail "rating --json says the number"
for wrong in 11 -1 3.5 five; do
    if cli rating "${wrong}" 2>/dev/null; then
        fail "rating ${wrong} fails"
    fi
done
[ "$(cli rating)" = "10" ] || fail "and a refused rating changes nothing"

# The album of what plays has its own rating.
[ "$(cli albumrating)" = "0" ] || fail "an unrated album says 0"
cli albumrating 8 | matches "^rated 8: " || fail "albumrating rates the album"
[ "$(cli albumrating)" = "8" ] || fail "albumrating reads it back"
[ "$(cli current --format '%albumrating%')" = "8" ] || fail "current formats the album's rating"
[ "$(cli rating)" = "10" ] || fail "and leaves the track's own alone"
cli albumrating 0 | matches "^cleared: " || fail "0 clears the album's"
[ "$(cli albumrating)" = "0" ] || fail "cleared, it says 0"

# No Last.fm account here: lastfm love says why and fails.
if cli lastfm love 2>"${work}/love.txt"; then
    fail "love without a Last.fm account fails"
fi
grep -q "lastfm.love" "${work}/love.txt" || fail "and says why"
if cli lastfm 2>/dev/null; then
    fail "lastfm wants love or unlove"
fi

# cover: none on these files, said and failed, into a file or a pipe.
if cli cover > /dev/null 2>"${work}/cover.txt"; then
    fail "cover of a file without one fails"
fi
matches "has no cover" "${work}/cover.txt" || fail "and says why"
if cli cover --size none "${work}/cover.jpg" 2>"${work}/cover.txt"; then
    fail "cover --size wants pixels"
fi
matches "a number of pixels" "${work}/cover.txt" || fail "and says so"

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
cli rating 4 > /dev/null
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
    job.finished | matches job.finished || fail "the second library is scanned"
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
last_all | matches "^${socket} playing .*gamma" || fail "watch --all starts on the engine that plays"
cli2 play track alpha 2>/dev/null > /dev/null
for _ in $(seq 1 100); do
    last_all | matches "^${second} playing .*alpha" && break
    sleep 0.05
done
last_all | matches "^${second} playing .*alpha" || fail "it follows the engine that started last"
cli2 stop > /dev/null
for _ in $(seq 1 100); do
    last_all | matches "^${socket} playing" && break
    sleep 0.05
done
last_all | matches "^${socket} playing .*gamma" || fail "and back to the one still playing"
kill "${watch_pid}" 2>/dev/null || true
wait "${watch_pid}" 2>/dev/null || true
kill "${second_pid}" 2>/dev/null || true
wait "${second_pid}" 2>/dev/null || true
second_pid=""

cli outputs | matches "^\*" || fail "outputs marks the one in use"
cli stop | matches "^stopped:" || fail "stop stops"

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
cli status | head -1 | matches -F "${library_says}" \
    || fail "a bare entry is shown as the library knows it (${library_says}), not by its name"
cli stop > /dev/null

# Chosen by the engine: random tracks, as many as asked, each once; and a
# whole album.
cli play random tracks 2 2>/dev/null | matches "^playing:" || fail "play random tracks plays"
cli --json status | python3 -c '
import json, sys
assert json.load(sys.stdin)["queue_size"] == 2' || fail "as many as asked for"
engine '{"id":11,"method":"playback.queue"}' | python3 -c '
import json, sys
paths = [entry["path"] for entry in json.load(sys.stdin)["result"]["entries"]]
assert len(set(paths)) == 2, paths' || fail "each track once"
cli play random album 2>/dev/null | matches "^playing:" || fail "play random album plays"
album_size="$(cli --json status | python3 -c 'import json,sys; print(json.load(sys.stdin)["queue_size"])')"
cli --json albums | python3 -c "
import json, sys
assert ${album_size} in [album['tracks'] for album in json.load(sys.stdin)]" \
    || fail "a whole album of the library (${album_size} tracks)"
if cli play random tracks 0 2>/dev/null; then
    fail "no tracks is refused"
fi
cli stop > /dev/null

# ADR-0233: the engine's lists, listed and played by name.
first="$(python3 -c 'import base64,sys; print(base64.b64encode(sys.argv[1].encode()).decode())' "${work}/music/first/alpha.wav")"
second="$(python3 -c 'import base64,sys; print(base64.b64encode(sys.argv[1].encode()).decode())' "${work}/music/second/gamma.wav")"
engine "{\"id\":20,\"method\":\"list.save\",\"params\":{\"name\":\"Road trip\",\"kind\":\"saved\",\"items\":[{\"path\":\"${second}\"},{\"path\":\"${first}\"}]}}" > /dev/null
engine "{\"id\":21,\"method\":\"list.save\",\"params\":{\"name\":\"Road work\",\"items\":[{\"path\":\"${first}\"}]}}" > /dev/null
cli lists > "${work}/lists.txt" || fail "lists lists"
grep -q "^Road trip (2 tracks)$" "${work}/lists.txt" || fail "a saved list, by name and size"
# ADR-0286: a list saved without a kind is a saved playlist too.
grep -q "^Road work (1 tracks)$" "${work}/lists.txt" || fail "every list is a saved playlist"
# ADR-0296: and each is a file in the playlist folder, which is said and chosen.
cli lists folder | head -1 | grep -q "/music/Playlists (the default)$" || fail "lists folder says the folder"
[ -f "${work}/music/Playlists/Road trip.m3u8" ] || fail "a saved list is its file"
grep -q "^second/gamma.wav$" "${work}/music/Playlists/Road trip.m3u8" \
    || fail "its tracks named as MPD names them"
mkdir -p "${work}/mpd-playlists"
cli lists folder "${work}/mpd-playlists" --style absolute | head -1 \
    | grep -q "/mpd-playlists$" || fail "lists folder chooses another"
grep -q "^${work}/music/second/gamma.wav$" "${work}/mpd-playlists/Road trip.m3u8" \
    || fail "the lists written there, in the style chosen"
cli lists folder default > /dev/null || fail "and back to the default"
# ADR-0342: completion asks the engine for its lists and speakers.
cli __complete lists | matches -x "Road trip" || fail "__complete lists names them"
[ -n "$(cli __complete outputs)" ] || fail "__complete outputs names the speakers"
completed() {
    # What bash completes for the words given, the last the one typed.
    PATH="$(dirname "${cli}"):${PATH}" MELODY_SERVER="${socket}" bash -c '
        source "$1"; shift
        COMP_WORDS=(melody-cli "$@"); COMP_CWORD=$#
        _melody_cli; printf "%s\n" "${COMPREPLY[@]}"' completion "${completion}" "$@"
}
completion="$(dirname "$0")/../packaging/completions/melody-cli.bash"
completed play list Road | matches -x -F 'Road\ trip' || fail "bash completes a list's name, one word"
completed up | matches -x upnext || fail "bash completes a command"
completed lastfm "" | matches -x unlove || fail "and a command's words"
completed config pass | matches -x password-file || fail "and melodyd.conf's keys"
cli --keys lists | matches -P "^Road trip \(2 tracks\)\t[0-9a-f-]{36}$" || fail "--keys gives each its id"
cli play list trip 2>/dev/null | matches "^playing:" || fail "play list plays one by a word of its name"
cli --json status | python3 -c '
import json, sys
assert json.load(sys.stdin)["queue_size"] == 2' || fail "the whole list is the queue"
if cli play list road 2>"${work}/ambiguous.txt"; then
    fail "a word two lists share is refused"
fi
grep -q "more than one list matches" "${work}/ambiguous.txt" || fail "and says which"
if cli play list nothing-like-this 2>/dev/null; then
    fail "a list that is not there fails"
fi
cli stop > /dev/null

# Wrong words say so, and fail.
if cli play album nothing-like-this 2>"${work}/none.txt"; then
    fail "an album that is not there fails"
fi
grep -q "no album matches" "${work}/none.txt" || fail "and says why"

# ADR-0223: an engine on the network wants its password, which may be kept
# in a file, as melodyd's own can.
port=$(( 20000 + RANDOM % 20000 ))
printf 'correct horse \r\nsecond line\n' > "${work}/password"
"${melodyd}" --socket "${work}/tcp.sock" --state "${work}/tcp-state" \
    --listen "127.0.0.1:${port}" --password "correct horse" 2>"${work}/tcp.log" &
second_pid=$!
for _ in $(seq 1 100); do
    grep -q "with a password" "${work}/tcp.log" 2>/dev/null && break
    sleep 0.05
done
grep -q "with a password" "${work}/tcp.log" || fail "an engine with a password starts"
tcp="127.0.0.1:${port}"
"${cli}" --server "${tcp}" --password-file "${work}/password" status > /dev/null ||
    fail "--password-file gives the file's first line, trimmed"
MELODY_PASSWORD_FILE="${work}/password" "${cli}" --server "${tcp}" status > /dev/null ||
    fail "and so does \$MELODY_PASSWORD_FILE"
MELODY_PASSWORD_FILE="${work}/nowhere" "${cli}" --server "${tcp}" --password "correct horse" \
    status > /dev/null || fail "--password on the line comes before the file in the environment"
if "${cli}" --server "${tcp}" --password wrong status > /dev/null 2>&1; then
    fail "a wrong password is refused"
fi
: > "${work}/empty"
if "${cli}" --server "${tcp}" --password-file "${work}/empty" status 2>"${work}/empty.txt"; then
    fail "a file without a password fails"
fi
grep -q "no password in" "${work}/empty.txt" || fail "and says why"
if "${cli}" --server "${tcp}" --password-file "${work}/nowhere" status 2>"${work}/nowhere.txt"; then
    fail "a file that is not there fails"
fi
grep -q "cannot read" "${work}/nowhere.txt" || fail "and says why"
kill "${second_pid}" 2>/dev/null || true
wait "${second_pid}" 2>/dev/null || true
second_pid=""

# ReplayGain, measured and written by the engine: an album found by its
# words, as play finds it. WAV has no tags, so the gains go to sidecars.
mkdir -p "${work}/music/tones"
python3 - "${work}/music/tones" <<'PY'
import math, struct, sys, wave
for name, level in (("one.wav", 0.5), ("two.wav", 0.25)):
    with wave.open(f"{sys.argv[1]}/{name}", "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(44100)
        out.writeframes(b"".join(struct.pack("<h", int(level * 32767 * math.sin(2 * math.pi * 440 * i / 44100)))
                                 for i in range(44100 * 3)))
PY
engine '{"id":20,"method":"job.submit","params":{"job":"catalogue.scan"}}' job.finished > /dev/null
# Untagged, the album is found by its key, which names its folder.
tones="$(cli --json albums | python3 -c '
import base64, json, sys
print(next(album["key"] for album in json.load(sys.stdin)
           if base64.b64decode(album["key"]).endswith(b"/tones")))')"
cli replaygain album --key "${tones}" >"${work}/replaygain.txt" 2>"${work}/replaygain.err" \
    || { cat "${work}/replaygain.err" >&2; fail "replaygain measures and writes an album"; }
[ "$(grep -c " album " "${work}/replaygain.txt")" = "2" ] \
    || { cat "${work}/replaygain.txt" >&2; fail "each track has its gain and the album's"; }
grep -q "wrote ReplayGain to 2 files" "${work}/replaygain.err" || fail "and says it wrote them"
[ -f "${work}/music/tones/one.wav.tkmeta" ] && [ -f "${work}/music/tones/two.wav.tkmeta" ] \
    || fail "into their sidecars"
cli --json replaygain track one | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["written"] == 0 and len(result["tracks"]) == 1, result
assert result["tracks"][0]["album_gain"] == "", result' \
    || fail "a track alone has no album gain, and unchanged it is not written again"
if cli replaygain album nothing-like-this 2>/dev/null; then
    fail "replaygain of nothing found fails"
fi

echo "melody-cli: ok"

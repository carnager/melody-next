#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0296: the engine's saved playlists are the files in its playlist folder
# -- a list saved, edited, renamed or deleted by a client is its file's; a
# file another program put there, changed or took away, the list's. MPD's
# files -- tracks named under the music folder -- are read and kept so.

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

mkdir -p "${work}/music/A" "${work}/elsewhere"
python3 - "${work}/music/A" <<'PY'
import sys, wave
for name in ("one", "two", "three"):
    with wave.open(f"{sys.argv[1]}/{name}.wav", "wb") as out:
        out.setnchannels(2); out.setsampwidth(2); out.setframerate(44100)
        out.writeframes(b"\0" * 44100 * 4)
PY

socket="${work}/melodyd.sock"
start() {
    "${melodyd}" --socket "${socket}" --state "${work}/state" --local-only \
        2>>"${work}/engine.log" &
    daemon_pid=$!
    for _ in $(seq 1 100); do
        [ -S "${socket}" ] && return 0
        sleep 0.05
    done
    fail "the engine starts"
}
stop() {
    kill "${daemon_pid}"
    wait "${daemon_pid}" 2>/dev/null || true
    daemon_pid=""
    rm -f "${socket}"
}

start
python3 - "${socket}" "${work}" <<'PY' || fail "the playlist folder is the playlists"
import base64, json, os, socket, sys, time

path, work = sys.argv[1], sys.argv[2]
music = f"{work}/music"
folder = f"{music}/Playlists"

class Client:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(10)
        self.s.connect(path)
        self.buffer = b""
        self.n = 0
    def line(self):
        while b"\n" not in self.buffer:
            chunk = self.s.recv(65536)
            if not chunk:
                raise SystemExit("the engine hung up")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)
    def call(self, method, params=None):
        self.n += 1
        self.s.sendall(json.dumps({"id": self.n, "method": method, "params": params or {}}).encode() + b"\n")
        while True:
            message = self.line()
            if message.get("id") == self.n:
                return message

def check(condition, what):
    if not condition:
        raise SystemExit("FAILED: " + what)

def encode(text):
    return base64.b64encode(text.encode()).decode()

def decode(text):
    return base64.b64decode(text).decode()

def read(name):
    with open(f"{folder}/{name}", encoding="utf-8") as file:
        return file.read()

def lists():
    return {l["name"]: l for l in c.call("list.all")["result"]["lists"]}

c = Client()
check("result" in c.call("catalogue.add_root", {"path": encode(music)}), "the music folder is added")
said = c.call("playlists.folder")["result"]
check(decode(said["folder"]) == folder and said["style"] == "root",
      "the folder is Playlists in the music folder, MPD's style")

# A list saved is its file: the tracks under the music folder, as MPD names them.
made = c.call("list.save", {"name": "Road/trip", "items": [
    {"path": encode(f"{music}/A/one.wav"), "title": "One", "artist": "Someone"},
    {"path": encode(f"{music}/A/two.wav"), "title": "Two"}]})["result"]
road = made["id"]
check(os.path.isdir(folder), "the folder is made when first needed")
text = read("Road_trip.m3u8")
check(text == f"#EXTM3U\n#MELODY-LIST:{road}\n#EXTINF:-1,Someone - One\nA/one.wav\n"
              "#EXTINF:-1,Two\nA/two.wav\n", "the saved list is written: " + repr(text))

# Edited by a client: the file follows.
got = c.call("list.get", {"id": road})["result"]
edited = c.call("list.edit", {"id": road, "revision": got["revision"], "edits": [
    {"insert": [{"entry": "7d1a6d8e-0c55-4a8e-9a43-2f2b0f6e1a01",
                 "path": encode(f"{music}/A/three.wav")}], "after": got["items"][1]["entry"]}]})
check("result" in edited, "an edit is taken: " + repr(edited))
check(read("Road_trip.m3u8").endswith("A/two.wav\nA/three.wav\n"), "and written")

# MPD's own file, put there: a list of it.
with open(f"{folder}/mix.m3u", "w") as file:
    file.write("A/three.wav\nA/one.wav\nA/gone.wav\nhttp://radio.example/\n")
c.call("playlists.sync")
mix = lists().get("mix")
check(mix and mix["kind"] == "saved" and mix["tracks"] == 3, "an MPD playlist dropped in is a list")
items = c.call("list.get", {"id": mix["id"]})["result"]["items"]
check([decode(i["path"]) for i in items] == [f"{music}/A/three.wav", f"{music}/A/one.wav",
                                             f"{music}/A/gone.wav"],
      "its tracks found under the music folder, a missing one kept")
kept_entry = items[1]["entry"]

# Changed by MPD: the list follows, the entries that stayed keep their identities.
time.sleep(0.01)
with open(f"{folder}/mix.m3u", "w") as file:
    file.write("A/one.wav\nA/two.wav\n")
c.call("playlists.sync")
items = c.call("list.get", {"id": mix["id"]})["result"]["items"]
check([decode(i["path"]) for i in items] == [f"{music}/A/one.wav", f"{music}/A/two.wav"],
      "an edit made outside is the list's")
check(items[0]["entry"] == kept_entry, "the entry that stayed keeps its identity")

# Edited here, it is still MPD's: no other style forced on it.
got = c.call("list.get", {"id": mix["id"]})["result"]
c.call("list.edit", {"id": mix["id"], "revision": got["revision"],
                     "edits": [{"remove": [got["items"][1]["entry"]]}]})
text = read("mix.m3u")
check("A/one.wav\n" in text and "A/two.wav" not in text and f"{music}/" not in text,
      "written back in its own style: " + repr(text))

# Renamed: so is the file. Deleted: the file goes aside, not away.
renamed = c.call("list.rename", {"id": road, "name": "Holiday"})
check("result" in renamed, "a rename is taken")
check(os.path.exists(f"{folder}/Holiday.m3u8") and not os.path.exists(f"{folder}/Road_trip.m3u8"),
      "the file is renamed with it")
check(c.call("list.delete", {"id": road})["result"]["deleted"], "a delete is taken")
check(not os.path.exists(f"{folder}/Holiday.m3u8") and
      os.path.exists(f"{folder}/.deleted/Holiday.m3u8"), "its file is set aside in .deleted")

# Taken away outside: the list goes.
os.remove(f"{folder}/mix.m3u")
c.call("playlists.sync")
check("mix" not in lists(), "a file taken away takes its list")

# A third, for the restart.
c.call("list.save", {"name": "Keep", "items": [{"path": encode(f"{music}/A/one.wav")}]})
check(os.path.exists(f"{folder}/Keep.m3u8"), "a new list is written")
PY

# After a restart: the same lists, none twice.
stop
start
python3 - "${socket}" "${work}" <<'PY' || fail "a restart keeps the folder's lists"
import base64, json, os, socket, sys, time
path, work = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(path)
buffer = b""; n = 0
def call(method, params=None):
    global buffer, n
    n += 1
    s.sendall(json.dumps({"id": n, "method": method, "params": params or {}}).encode() + b"\n")
    while True:
        while b"\n" not in buffer:
            buffer += s.recv(65536)
        line, buffer = buffer.split(b"\n", 1)
        message = json.loads(line)
        if message.get("id") == n:
            return message
def check(condition, what):
    if not condition:
        raise SystemExit("FAILED: " + what)
call("playlists.sync")
names = sorted(l["name"] for l in call("list.all")["result"]["lists"])
check(names == ["Keep"], "one list, as before: " + repr(names))

# Another folder: the lists are written there, and its files read.
other = f"{work}/elsewhere"
with open(f"{other}/old.m3u8", "w") as file:
    file.write(f"#EXTM3U\n{work}/music/A/two.wav\n")
said = call("playlists.set_folder", {"folder": base64.b64encode(other.encode()).decode(),
                                     "style": "absolute"})["result"]
check(base64.b64decode(said["folder"]).decode() == other and said["style"] == "absolute",
      "another folder is chosen")
check(os.path.exists(f"{other}/Keep.m3u8"), "the lists are written to it")
names = sorted(l["name"] for l in call("list.all")["result"]["lists"])
check(names == ["Keep", "old"], "and its files read: " + repr(names))
with open(f"{other}/Keep.m3u8") as file:
    check(f"{work}/music/A/one.wav" in file.read(), "in the style chosen")

# A folder not there -- not mounted -- drops nothing.
os.rename(other, f"{work}/unmounted")
call("playlists.sync")
names = sorted(l["name"] for l in call("list.all")["result"]["lists"])
check(names == ["Keep", "old"], "a folder gone drops no list: " + repr(names))
check(not os.path.exists(other), "nor is it made again")
PY

echo "engine playlist folder: ok"

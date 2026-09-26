#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0233: the engine's lists over the wire, as any client uses them --
# working and saved, revisions refusing a stale write, a change heard by a
# second client, and a list played as the queue.

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

python3 - "${work}" <<'PY'
import sys, wave
for name in ("one", "two"):
    with wave.open(f"{sys.argv[1]}/{name}.wav", "wb") as out:
        out.setnchannels(2); out.setsampwidth(2); out.setframerate(44100)
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

python3 - "${socket}" "${work}" <<'PY' || fail "the lists behave as ADR-0233 says"
import base64, json, socket, sys, time

path, work = sys.argv[1], sys.argv[2]

class Client:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(10)
        self.s.connect(path)
        self.buffer = b""
        self.events = []
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
            if "event" in message:
                self.events.append(message)
    def wait_event(self, name, seconds=5):
        deadline = time.time() + seconds
        while time.time() < deadline:
            for event in self.events:
                if event["event"] == name:
                    self.events.remove(event)
                    return event
            try:
                message = self.line()
            except socket.timeout:
                break
            if "event" in message:
                self.events.append(message)
        return None

def check(condition, what):
    if not condition:
        raise SystemExit("FAILED: " + what)

def encode(text):
    return base64.b64encode(text.encode()).decode()

a, b = Client(), Client()
b.call("playback.state")  # b listens from here on

check(a.call("list.all")["result"]["lists"] == [], "no lists at first")
items = [{"path": encode(f"{work}/one.wav"), "title": "One", "artist": "Someone", "album": "Album"},
         {"path": encode(f"{work}/two.wav"), "segment": {"start_sample": 0, "end_sample": 44100},
          "selection": {"stream_index": None, "subsong_index": 1}, "duration_ms": 1000}]
created = a.call("list.save", {"name": "Untitled", "items": items})["result"]
check(created["kind"] == "working" and created["revision"] == 1 and created["tracks"] == 2,
      "a list saved without a kind is a working list, at revision 1")
list_id = created["id"]
event = b.wait_event("list.changed")
check(event and event["data"]["id"] == list_id and event["data"]["revision"] == 1,
      "another client hears of the new list")

got = a.call("list.get", {"id": list_id})["result"]
check([i["title"] for i in got["items"]] == ["One", ""], "the items come back in order")
check(got["items"][1]["segment"] == {"start_sample": 0, "end_sample": 44100} and
      got["items"][1]["selection"]["subsong_index"] == 1, "with their segment and selection")
entries = [i["entry"] for i in got["items"]]

saved = a.call("list.save", {"id": list_id, "name": "Road trip", "kind": "saved",
                             "items": got["items"], "revision": 1})["result"]
check(saved["kind"] == "saved" and saved["revision"] == 2 and saved["name"] == "Road trip",
      "saving by name makes it a saved list")
check([i["entry"] for i in a.call("list.get", {"id": list_id})["result"]["items"]] == entries,
      "entries keep their identities through a save")

stale = a.call("list.save", {"id": list_id, "name": "Stale", "items": [], "revision": 1})
check(stale.get("error", {}).get("code") == "conflict", "a write against an old revision is refused")
check(a.call("list.rename", {"id": list_id, "name": "Stale", "revision": 1})["error"]["code"]
      == "conflict", "so is a rename")

played = a.call("list.play", {"id": list_id, "entry": entries[1]})
check("result" in played and played["result"]["playing"] == entries[1],
      "a list plays from the entry named")
queue = a.call("playback.queue")["result"]["entries"]
check([e["entry"] for e in queue] == entries, "and becomes the queue, same identities")

renamed = a.call("list.rename", {"id": list_id, "name": "Holiday"})["result"]
check(renamed["name"] == "Holiday" and renamed["revision"] == 3, "a rename is a write")
check(a.call("list.delete", {"id": list_id, "revision": 2})["error"]["code"] == "conflict",
      "a delete against an old revision is refused")
check(a.call("list.delete", {"id": list_id, "revision": 3})["result"]["deleted"] is True,
      "and at the current one it deletes")
gone = None
for _ in range(6):
    gone = b.wait_event("list.changed")
    if gone and gone["data"].get("deleted"):
        break
check(gone and gone["data"]["deleted"] is True, "the other client hears it went")
check(a.call("list.get", {"id": list_id})["error"]["code"] == "not_found", "and it is gone")
check(a.call("list.delete", {"id": list_id})["result"]["deleted"] is False,
      "deleting it again is no error")
check(a.call("list.save", {"name": "", "items": []})["error"]["code"] == "invalid_argument",
      "a list needs a name")

# Files moved: every list naming them follows, in one go, and says so.
one = a.call("list.save", {"name": "One", "kind": "saved",
                           "items": [{"path": encode(f"{work}/one.wav")},
                                     {"path": encode(f"{work}/two.wav")}]})["result"]
other = a.call("list.save", {"name": "Other", "kind": "saved",
                             "items": [{"path": encode(f"{work}/one.wav")}]})["result"]
untouched = a.call("list.save", {"name": "Untouched", "kind": "saved",
                                 "items": [{"path": encode(f"{work}/two.wav")}]})["result"]
moved = a.call("list.relocate", {"moves": [{"from": encode(f"{work}/one.wav"),
                                            "to": encode(f"{work}/moved/one.wav")}]})["result"]
check(sorted(moved["changed"]) == sorted([one["id"], other["id"]]),
      "the lists naming the file are the ones changed")
paths = [base64.b64decode(i["path"]).decode()
         for i in a.call("list.get", {"id": one["id"]})["result"]["items"]]
check(paths == [f"{work}/moved/one.wav", f"{work}/two.wav"], "the entry names the new path, in place")
check(a.call("list.get", {"id": other["id"]})["result"]["revision"] == other["revision"] + 1,
      "each list changed gets a new revision")
check(a.call("list.get", {"id": untouched["id"]})["result"]["revision"] == untouched["revision"],
      "a list without the file is left alone")
heard = set()
for _ in range(10):
    event = b.wait_event("list.changed", 1)
    if event is None:
        break
    heard.add(event["data"]["id"])
check({one["id"], other["id"]} <= heard, "and other clients hear of both")
PY

echo "engine lists: ok"

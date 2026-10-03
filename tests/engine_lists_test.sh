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

# ADR-0256: a list changes by edits, and the queue played from it follows.
import uuid
edited = a.call("list.save", {"name": "Edited", "items": [{"path": encode(f"{work}/one.wav")},
                                                          {"path": encode(f"{work}/two.wav")}]})["result"]
first, second = [i["entry"] for i in a.call("list.get", {"id": edited["id"]})["result"]["items"]]
check("result" in a.call("list.play", {"id": edited["id"], "entry": second}), "the list plays")
fresh = str(uuid.uuid4())
changed = a.call("list.edit", {"id": edited["id"], "revision": edited["revision"], "edits": [
    {"insert": [{"entry": fresh, "path": encode(f"{work}/one.wav"), "title": "Again",
                 "album_artist": "Various", "date": "1999",
                 "replay_gain": {"track_gain_db": -6.5}}], "after": None},
    {"remove": [first]}]})
check("result" in changed and changed["result"]["revision"] == edited["revision"] + 1,
      "an edit answers the next revision: " + json.dumps(changed))
stored = a.call("list.get", {"id": edited["id"]})["result"]["items"]
check([i["entry"] for i in stored] == [fresh, second], "the edits are applied in order")
check(stored[0]["album_artist"] == "Various" and stored[0]["date"] == "1999" and
      stored[0]["replay_gain"]["track_gain_db"] == -6.5, "an item keeps what a queue entry needs")
queue = a.call("playback.queue")["result"]["entries"]
check([e["entry"] for e in queue] == [fresh, second], "the queue follows the list it was played from")
check(queue[0]["group"]["album_artist"] == "Various" and queue[0]["replay_gain"]["track_gain_db"] == -6.5,
      "with the item's album artist and ReplayGain")
check(a.call("playback.state")["result"]["entry"] == second, "and what plays keeps playing")
moved = a.call("list.edit", {"id": edited["id"], "revision": edited["revision"] + 1,
                             "edits": [{"move": [second], "after": None}]})
check("result" in moved, "a move is an edit")
check([e["entry"] for e in a.call("playback.queue")["result"]["entries"]] == [second, fresh],
      "and the queue moves with it")
stale = a.call("list.edit", {"id": edited["id"], "revision": edited["revision"],
                             "edits": [{"remove": [second]}]})
check(stale.get("error", {}).get("code") == "conflict", "an edit against an old revision is refused")
unknown = a.call("list.edit", {"id": edited["id"], "revision": edited["revision"] + 2,
                               "edits": [{"remove": [second]}, {"remove": [str(uuid.uuid4())]}]})
check(unknown.get("error", {}).get("code") == "not_found", "an edit of an unknown entry is refused")
check(a.call("list.get", {"id": edited["id"]})["result"]["revision"] == edited["revision"] + 2 and
      len(a.call("playback.queue")["result"]["entries"]) == 2, "and nothing of it is applied")
renamed = a.call("list.edit", {"id": edited["id"], "revision": edited["revision"] + 2, "edits": [],
                               "name": "Kept", "kind": "saved"})["result"]
check(renamed["name"] == "Kept" and renamed["kind"] == "saved" and
      renamed["revision"] == edited["revision"] + 3, "a name and a kind are edits too")
check(a.call("list.edit", {"id": edited["id"], "edits": []})["error"]["code"] == "invalid_argument",
      "an edit names the revision it was worked out from")

# ADR-0259: a saved list's unsaved edits are a draft on the engine, which
# every client sees, and saving it writes the list.
kept = a.call("list.save", {"name": "Kept", "kind": "saved", "items": [
    {"path": encode(f"{work}/one.wav")}, {"path": encode(f"{work}/two.wav")}]})["result"]
draft = a.call("list.draft", {"of": kept["id"]})["result"]
check(draft["kind"] == "working" and draft["draft_of"] == kept["id"] and
      draft["draft_base"] == kept["revision"] and draft["tracks"] == 2,
      "a draft is a working list naming the saved list and its revision")
check(a.call("list.draft", {"of": kept["id"]})["result"]["id"] == draft["id"],
      "a saved list has one draft")
def heard_of(client, ids, seconds=5):
    """What `client` hears of the lists `ids`: each id's deleted flag."""
    found = {}
    deadline = time.time() + seconds
    while set(found) != set(ids) and time.time() < deadline:
        event = client.wait_event("list.changed", 1)
        if event and event["data"]["id"] in ids:
            found[event["data"]["id"]] = event["data"]["deleted"]
    return found

def drain(client):
    """Reads what `client` has been told so far, so what follows is news."""
    client.s.settimeout(0.3)
    try:
        while client.wait_event("list.changed", 0.3):
            pass
    finally:
        client.s.settimeout(10)
    client.events.clear()

check(heard_of(b, [draft["id"]]) == {draft["id"]: False}, "another client hears of the draft")
check(a.call("list.draft", {"of": draft["id"]})["error"]["code"] == "invalid_argument",
      "a draft has no draft")
d_entries = [i["entry"] for i in a.call("list.get", {"id": draft["id"]})["result"]["items"]]
check(d_entries == [i["entry"] for i in a.call("list.get", {"id": kept["id"]})["result"]["items"]],
      "with the saved list's entries")
check("result" in a.call("list.play", {"id": draft["id"], "entry": d_entries[0]}),
      "the draft plays")
edited_draft = a.call("list.edit", {"id": draft["id"], "revision": draft["revision"],
                                    "edits": [{"move": [d_entries[1]], "after": None}],
                                    "name": "Kept, reordered"})
check("result" in edited_draft, "the draft is edited")
drain(b)
committed = a.call("list.commit", {"id": draft["id"]})["result"]
check(committed["id"] == kept["id"] and committed["revision"] == kept["revision"] + 1 and
      committed["name"] == "Kept, reordered" and committed["draft_of"] is None,
      "saving the draft writes the list it drafts")
check([i["entry"] for i in a.call("list.get", {"id": kept["id"]})["result"]["items"]] ==
      [d_entries[1], d_entries[0]], "with the draft's entries")
check(a.call("list.get", {"id": draft["id"]})["error"]["code"] == "not_found", "and the draft goes")
check(heard_of(b, [draft["id"], kept["id"]]) == {draft["id"]: True, kept["id"]: False},
      "another client hears both")
moved_back = a.call("list.edit", {"id": kept["id"], "revision": committed["revision"],
                                  "edits": [{"move": [d_entries[0]], "after": None}]})
check("result" in moved_back and
      [e["entry"] for e in a.call("playback.queue")["result"]["entries"]] == d_entries,
      "the queue played from the draft follows the saved list now")
stale = a.call("list.draft", {"of": kept["id"]})["result"]
check("result" in a.call("list.save", {"id": kept["id"], "name": "Kept", "kind": "saved",
                                       "items": [{"path": encode(f"{work}/one.wav")}]}),
      "someone saves the list itself")
check(a.call("list.commit", {"id": stale["id"]})["error"]["code"] == "conflict",
      "saving a draft of an older revision is a conflict")
check("result" in a.call("list.commit", {"id": stale["id"], "force": True}),
      "unless it is kept regardless")
last = a.call("list.draft", {"of": kept["id"]})["result"]
drain(b)
check(a.call("list.delete", {"id": kept["id"]})["result"]["deleted"] is True, "the list is deleted")
check(a.call("list.get", {"id": last["id"]})["error"]["code"] == "not_found", "and its draft")
check(heard_of(b, [kept["id"], last["id"]]) == {kept["id"]: True, last["id"]: True},
      "and both are told as gone")

# ADR-0259: asked to, the engine describes each item from its library -- a
# file it indexes with its tags and revision, one it does not as saved.
music = f"{work}/music"
import os, shutil
os.makedirs(music)
shutil.copy(f"{work}/one.wav", f"{music}/one.wav")
check("result" in a.call("catalogue.add_root", {"path": encode(music)}), "a folder is added")
check("result" in a.call("job.submit", {"job": "catalogue.scan"}), "and scanned")
check(a.wait_event("job.finished", 20) is not None, "the scan finishes")
described = a.call("list.save", {"name": "Described", "items": [
    {"path": encode(f"{music}/one.wav"), "title": "Snapshot"},
    {"path": encode(f"{work}/two.wav"), "title": "Outside"}]})["result"]
plain = a.call("list.get", {"id": described["id"]})["result"]["items"]
check(all("library" not in i for i in plain), "unasked, nothing is described")
items = a.call("list.get", {"id": described["id"], "describe": True})["result"]["items"]
indexed, outside = items
library = indexed.get("library")
check(library is not None and library["duration_ms"] == 30000 and library["codec"] != "",
      "an indexed file is described from the library")
check(library["revision"] is not None and library["revision"]["size"] == os.path.getsize(f"{music}/one.wav"),
      "with the revision it was indexed at")
check(isinstance(library["fields"], dict), "and its tags")
check("library" not in outside and outside["title"] == "Outside",
      "a file the library does not index keeps what it was saved with")

# ADR-0259: a kept search made on the engine, from its library.
everything = a.call("list.from_query", {"query": "ALL", "name": "Everything"})["result"]
check(everything["kind"] == "working" and everything["tracks"] == 1 and
      everything["name"] == "Everything", "a query's matches become a working list")
made = a.call("list.get", {"id": everything["id"]})["result"]["items"]
check([i["path"] for i in made] == [encode(f"{music}/one.wav")] and made[0]["duration_ms"] == 30000,
      "of the files the library indexes, named from it")
nothing = a.call("list.from_query", {"query": "title IS nothing-here", "name": "None"})["result"]
check(nothing["tracks"] == 0, "a query matching nothing makes an empty list")
check(a.call("list.from_query", {"query": "codec (((", "name": "Bad"})["error"]["code"] ==
      "invalid_argument", "a query that does not compile is refused")
check("result" in a.call("list.from_query", {"query": "codec (((", "name": "Words", "words": True}),
      "unless it is a word search, which any text is")
PY

echo "engine lists: ok"

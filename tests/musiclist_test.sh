#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# melody-musiclist against a real engine: every album with its rating and its
# computed rating, as a page, uploaded with scp.

set -euo pipefail

melodyd="$1"
cli="$2"
script="$3"
work="$(mktemp -d)"
unset MELODY_SERVER MELODY_PASSWORD MELODY_PASSWORD_FILE
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

# Two albums of tagged silence: RIFF INFO tags, which the library reads.
mkdir -p "${work}/music/first" "${work}/music/second"
python3 - "${work}/music" <<'PY'
import struct, sys, wave
def tagged(path, title, artist, album, date):
    with wave.open(path, "wb") as out:
        out.setnchannels(2); out.setsampwidth(2); out.setframerate(44100)
        out.writeframes(b"\0" * 44100 * 4)
    entries = b""
    for key, value in (("INAM", title), ("IART", artist), ("IPRD", album), ("ICRD", date)):
        text = value.encode() + b"\0"
        if len(text) % 2:
            text += b"\0"
        entries += key.encode() + struct.pack("<I", len(text)) + text
    chunk = b"LIST" + struct.pack("<I", 4 + len(entries)) + b"INFO" + entries
    with open(path, "r+b") as out:
        out.seek(0, 2)
        out.write(chunk)
        size = out.tell()
        out.seek(4)
        out.write(struct.pack("<I", size - 8))
root = sys.argv[1]
tagged(f"{root}/first/alpha.wav", "Alpha", "First Artist", "First Album", "1999")
tagged(f"{root}/first/beta.wav", "Beta", "First Artist", "First Album", "1999")
tagged(f"{root}/second/gamma.wav", "Gamma", "Second Artist", "Second Album", "2004")
PY

socket="${work}/melodyd.sock"
"${melodyd}" --socket "${socket}" --state "${work}/state" --local-only 2>"${work}/engine.log" &
daemon_pid=$!
for _ in $(seq 1 100); do
    [ -S "${socket}" ] && break
    sleep 0.05
done
[ -S "${socket}" ] || fail "the engine starts"

# Scanned, and rated as a listener would: the first album's tracks 8 and 6,
# the album itself 9; the second album not at all.
python3 - "${socket}" "${work}/music" "${cli}" <<'PY' || fail "the library is scanned and rated"
import base64, json, socket, subprocess, sys
path, music, cli = sys.argv[1:4]
connection = socket.socket(socket.AF_UNIX)
connection.settimeout(30)
connection.connect(path)
reader = connection.makefile("rb")
def call(number, method, params):
    connection.sendall(json.dumps({"id": number, "method": method, "params": params}).encode() + b"\n")
    while True:
        message = json.loads(reader.readline())
        if method == "job.submit" and message.get("event") == "job.finished":
            return message
        if method != "job.submit" and message.get("id") == number:
            assert "error" not in message, message
            return message
call(1, "catalogue.add_root", {"path": base64.b64encode(music.encode()).decode()})
call(2, "job.submit", {"job": "catalogue.scan"})
tracks = json.loads(subprocess.run([cli, "--server", path, "--json", "tracks"],
                                   capture_output=True, text=True, check=True).stdout)
by_title = {track["title"]: track for track in tracks}
call(3, "catalogue.set_rating", {"hash": by_title["Alpha"]["rating_hash"], "rating": 8})
call(4, "catalogue.set_rating", {"hash": by_title["Beta"]["rating_hash"], "rating": 6})
call(5, "catalogue.set_rating",
     {"hash": by_title["Alpha"]["album_rating_hash"], "album": True, "rating": 9})
PY

export MELODY_CLI="${cli}"
export XDG_CONFIG_HOME="${work}/config"
mkdir -p "${XDG_CONFIG_HOME}/melody"

# A first run writes a config to fill in, and says what is missing.
if python3 "${script}" > "${work}/first-run.log" 2>&1; then
    fail "without an upload destination it refuses"
fi
grep -q "configure both \[upload\].host and \[upload\].path" "${work}/first-run.log" \
    || fail "and says which"
grep -q '^\[engine\]' "${XDG_CONFIG_HOME}/melody/melody-musiclist.toml" \
    || fail "the config it writes names the engine"

cat > "${XDG_CONFIG_HOME}/melody/melody-musiclist.toml" <<TOML
[engine]
server = "${socket}"

[upload]
host = "webhost"
path = "/srv/list"

[output]
temp_file = "${work}/musiclist.html"
TOML

albums_of() {
    python3 - "$1" <<'PY'
import json, re, sys
page = open(sys.argv[1], encoding="utf-8").read()
found = re.search(r"const allAlbums = (.*);\n", page)
assert found, "the page carries its albums"
for album in sorted(json.loads(found.group(1)), key=lambda a: a["album"]):
    print(f'{album["artist"]}|{album["album"]}|{album["year"]}|{album["year_int"]}|'
          f'{album["rating"]}|{album["computed"]}')
PY
}

python3 "${script}" --output "${work}/page.html" > /dev/null || fail "--output writes the page"
expected="First Artist|First Album|1999|1999|9|7.0
Second Artist|Second Album|2004|2004|0|0.0"
[ "$(albums_of "${work}/page.html")" = "${expected}" ] \
    || fail "every album, its rating, and the mean of its tracks' once enough are rated: $(albums_of "${work}/page.html")"
grep -q "<title>Music Collection</title>" "${work}/page.html" || fail "as the page it always was"
python3 - "${work}/page.html" <<'PY' || fail "each album says when the library first had it, for Latest first"
import json, re, sys, time
page = open(sys.argv[1], encoding="utf-8").read()
albums = json.loads(re.search(r"const allAlbums = (.*);\n", page).group(1))
assert all(0 < album["added"] <= time.time() + 60 for album in albums), albums
assert 'id="latestToggle"' in page
PY
[ ! -e "${work}/musiclist.html" ] || fail "--output copies nothing"

# Uploaded with scp, to <path>/index.html, and the temporary file removed.
mkdir -p "${work}/bin"
cat > "${work}/bin/scp" <<SH
#!/bin/sh
printf '%s\n' "\$@" > "${work}/scp-arguments"
cp "\$1" "${work}/uploaded.html"
SH
chmod +x "${work}/bin/scp"
PATH="${work}/bin:${PATH}" python3 "${script}" > "${work}/upload.log" || fail "the page is uploaded"
[ "$(sed -n 2p "${work}/scp-arguments")" = "webhost:/srv/list/index.html" ] \
    || fail "to the configured place"
[ "$(albums_of "${work}/uploaded.html")" = "${expected}" ] || fail "with the albums"
[ ! -e "${work}/musiclist.html" ] || fail "and the temporary file is gone"

echo "melody-musiclist: ok"

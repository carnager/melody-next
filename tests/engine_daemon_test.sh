#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0220: melodyd as a process. A shell script is the right shape for this
# one -- it exercises the daemon the way a person or an init system does, and
# it is itself evidence for the "a shell script remains a debugging tool"
# requirement.

set -euo pipefail

binary="$1"
work="$(mktemp -d)"
socket="${work}/melodyd.sock"
state="${work}/state"
daemon_pid=""

cleanup() {
    if [ -n "${daemon_pid}" ] && kill -0 "${daemon_pid}" 2>/dev/null; then
        kill "${daemon_pid}" 2>/dev/null || true
        wait "${daemon_pid}" 2>/dev/null || true
    fi
    rm -rf "${work}"
}
trap cleanup EXIT

fail() { echo "FAILED: $1" >&2; exit 1; }

# --help must work without touching any state, so it stays usable when the
# databases are unreadable.
"${binary}" --help > "${work}/help.txt" 2>&1 || fail "--help must succeed"
grep -q "protocol v1" "${work}/help.txt" || fail "--help must say what it speaks"
# The debugging idiom has to actually work, so the help must show a usable one.
grep -q -- "-w" "${work}/help.txt" || fail "--help must show a read timeout for nc"

"${binary}" --nonsense > "${work}/bad.txt" 2>&1 && fail "an unknown argument must fail" || true
grep -q "unrecognised argument" "${work}/bad.txt" || fail "and say which"

"${binary}" --socket "${socket}" --state "${state}" 2>"${work}/log.txt" &
daemon_pid=$!

for _ in $(seq 1 100); do
    [ -S "${socket}" ] && break
    sleep 0.05
done
[ -S "${socket}" ] || fail "the daemon must create its socket"

# The database is created on start, so a first run does not fail on a
# client's first request. One file: catalogue, ratings, lists and journals.
[ -f "${state}/lists.sqlite" ] || fail "the database must be created"
[ -f "${state}/library.sqlite3" ] && fail "the catalogue must not be a second database"

# A half-closed connection stays open by design, so nc needs a read timeout
# rather than waiting for a close that is not coming. -q is GNU netcat only
# and is silently ignored by the OpenBSD one, so the timeout is external.
answer="$(printf '{"id":1,"method":"catalogue.roots"}\n' | timeout 5 nc -U "${socket}" || true)"
echo "${answer}" | grep -q '"id":1' || fail "the daemon must answer the id asked"
echo "${answer}" | grep -q '"roots":\[\]' || fail "a fresh catalogue has no roots"

# A job streams its events down the same connection that submitted it.
job="$(printf '{"id":2,"method":"job.submit","params":{"job":"catalogue.scan"}}\n' \
    | timeout 5 nc -U "${socket}" || true)"
echo "${job}" | grep -q '"job_id"' || fail "submitting must answer with an identity"
echo "${job}" | grep -q '"event":"job.finished"' || fail "and the job must report a finish"

# A second engine must not steal a live socket, nor share the database.
if "${binary}" --socket "${socket}" --state "${work}/other" > "${work}/second.txt" 2>&1; then
    fail "a second daemon must refuse an occupied socket"
fi
grep -q -e "could not listen" -e "another engine" "${work}/second.txt" || fail "and say so"
if "${binary}" --socket "${work}/other.sock" --state "${state}" > "${work}/third.txt" 2>&1; then
    fail "a second daemon must refuse a database in use"
fi
grep -q "another engine is using" "${work}/third.txt" || fail "and say which"

# SIGTERM is how an init system stops it; the socket must not be left behind.
# Promptly, too: sampling workers once slept out their interval first, which
# made every stop and restart take five seconds.
stop_started=$(date +%s%N)
kill "${daemon_pid}"
wait "${daemon_pid}" 2>/dev/null || true
stop_ms=$(( ($(date +%s%N) - stop_started) / 1000000 ))
[ "${stop_ms}" -lt 1000 ] || fail "SIGTERM must stop the engine promptly (took ${stop_ms} ms)"
daemon_pid=""
[ -S "${socket}" ] && fail "the socket must be removed on shutdown"

# ADR-0223: TCP, opt-in, with a password when one is set. Also typed by
# hand: the handshake is one more line in nc, not a binary preamble.
port=$(( 20000 + RANDOM % 20000 ))
printf 'correct horse\n' > "${work}/password"
"${binary}" --socket "${socket}" --state "${state}" --listen "127.0.0.1:${port}" \
    --password-file "${work}/password" 2>"${work}/tcp-log.txt" &
daemon_pid=$!
for _ in $(seq 1 100); do
    grep -q "with a password" "${work}/tcp-log.txt" 2>/dev/null && break
    sleep 0.05
done
grep -q "with a password" "${work}/tcp-log.txt" || fail "--listen must say it wants a password"
[ -f "${state}/engine.token" ] && fail "no token file is made any more"

denied="$(printf '{"id":1,"method":"catalogue.roots"}\n' | timeout 5 nc 127.0.0.1 "${port}" || true)"
echo "${denied}" | grep -q '"unauthorized"' || fail "TCP without the password must be refused"

admitted="$(printf '{"id":1,"method":"session.authenticate","params":{"password":"correct horse"}}\n{"id":2,"method":"catalogue.roots"}\n' \
    | timeout 5 nc 127.0.0.1 "${port}" || true)"
echo "${admitted}" | grep -q '"authenticated":true' || fail "the right password must be accepted"
echo "${admitted}" | grep -q '"roots":\[\]' || fail "and the engine must then answer over TCP"

# The unix socket is unchanged: no handshake.
plain="$(printf '{"id":1,"method":"catalogue.roots"}\n' | timeout 5 nc -U "${socket}" || true)"
echo "${plain}" | grep -q '"roots":\[\]' || fail "the unix socket must need no token"

kill "${daemon_pid}"
wait "${daemon_pid}" 2>/dev/null || true
daemon_pid=""

# And without one, open: nothing to type, as with MPD.
"${binary}" --socket "${socket}" --state "${state}" --listen "127.0.0.1:${port}" \
    2>"${work}/open-log.txt" &
daemon_pid=$!
for _ in $(seq 1 100); do
    grep -q "no password" "${work}/open-log.txt" 2>/dev/null && break
    sleep 0.05
done
open="$(printf '{"id":1,"method":"catalogue.roots"}\n' | timeout 5 nc 127.0.0.1 "${port}" || true)"
echo "${open}" | grep -q '"roots":\[\]' || fail "TCP without a password set must be open"
kill "${daemon_pid}"
wait "${daemon_pid}" 2>/dev/null || true
daemon_pid=""

echo "engine daemon: ok"

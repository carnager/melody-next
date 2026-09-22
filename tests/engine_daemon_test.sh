#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0220: tkengine as a process. A shell script is the right shape for this
# one -- it exercises the daemon the way a person or an init system does, and
# it is itself evidence for the "a shell script remains a debugging tool"
# requirement.

set -euo pipefail

binary="$1"
work="$(mktemp -d)"
socket="${work}/tkengine.sock"
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

# Databases are created on start, so a first run does not fail on a client's
# first request.
[ -f "${state}/library.sqlite3" ] || fail "the catalogue must be created"
[ -f "${state}/workspace.sqlite3" ] || fail "the workspace must be created"

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

# A second engine must not steal a live socket.
if "${binary}" --socket "${socket}" --state "${state}" > "${work}/second.txt" 2>&1; then
    fail "a second daemon must refuse an occupied socket"
fi
grep -q "could not listen" "${work}/second.txt" || fail "and say so"

# SIGTERM is how an init system stops it; the socket must not be left behind.
kill "${daemon_pid}"
wait "${daemon_pid}" 2>/dev/null || true
daemon_pid=""
[ -S "${socket}" ] && fail "the socket must be removed on shutdown"

echo "engine daemon: ok"

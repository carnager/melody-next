#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Engines finding each other with nothing configured: one engine listening
# announces itself; another started with --agent, and a melody-agent with no
# --server, find it and become its outputs.

set -euo pipefail

melodyd="$1"
agent="$2"
work="$(mktemp -d)"
pids=()

cleanup() {
    for pid in "${pids[@]}"; do
        kill "${pid}" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf "${work}"
}
trap cleanup EXIT

fail() {
    echo "FAILED: $1" >&2
    for log in "${work}"/*.log; do echo "--- ${log}" >&2; cat "${log}" >&2; done
    exit 1
}

# A private service per run, so nothing real on this network is found or
# finds these (the test environment sets one too; this makes it unique).
export TRACKKNIFE_DISCOVERY_SERVICE="_mt-$$-${RANDOM}._tcp"
port=$((20000 + RANDOM % 20000))
password="discovery-test-$$"

# Reachable where it announces itself: on this machine's addresses, so a
# password rather than an open port for the moments this runs.
"${melodyd}" --socket "${work}/alpha.sock" --state "${work}/alpha" --name alpha \
    --listen "0.0.0.0:${port}" --password "${password}" 2>"${work}/alpha.log" &
pids+=($!)
for _ in $(seq 1 100); do
    [ -S "${work}/alpha.sock" ] && break
    sleep 0.05
done
[ -S "${work}/alpha.sock" ] || fail "alpha starts"
grep -q "not announced" "${work}/alpha.log" && { echo "discovery: no multicast here; skipping"; exit 0; }

"${melodyd}" --socket "${work}/beta.sock" --state "${work}/beta" --name beta \
    --local-only --agent --agent-password "${password}" 2>"${work}/beta.log" &
pids+=($!)
"${agent}" --name gamma --password "${password}" 2>"${work}/gamma.log" &
pids+=($!)

outputs() {
    printf '{"id":1,"method":"outputs.list"}\n' | timeout 3 nc -U "${work}/alpha.sock" || true
}
for _ in $(seq 1 150); do
    listed="$(outputs)"
    if echo "${listed}" | grep -q '"id":"agent:beta"' && echo "${listed}" | grep -q '"id":"agent:gamma"'; then
        break
    fi
    sleep 0.1
done
if grep -q "no audio here" "${work}/beta.log" "${work}/gamma.log"; then
    echo "discovery: no audio output here; skipping"
    exit 0
fi
echo "${listed}" | grep -q '"id":"agent:beta"' || fail "an engine with --agent finds alpha and plays for it"
echo "${listed}" | grep -q '"id":"agent:gamma"' || fail "a melody-agent with no --server finds alpha"
grep -q "playing for alpha" "${work}/beta.log" || fail "beta says whom it plays for"
# Not for itself: beta does not listen, so is not announced; alpha, which
# does, is not among its own outputs.
echo "${listed}" | grep -q '"id":"agent:alpha"' && fail "an engine does not play for itself"
echo "discovery: engines found each other"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# ADR-0286: melody-cli config reads and changes melodyd.conf, checked as
# melodyd reads it, comments kept, and --edit takes only what it would.

set -euo pipefail

cli="$1"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
export MELODYD_CONFIG="${work}/melody/melodyd.conf"
# Never the editor of whoever runs the tests.
unset VISUAL

fail() {
    echo "FAILED: $1" >&2
    [ -f "${MELODYD_CONFIG}" ] && cat "${MELODYD_CONFIG}" >&2
    exit 1
}

# No file yet: said, and nothing made.
"${cli}" config 2>"${work}/said"
grep -q "No ${MELODYD_CONFIG} yet" "${work}/said" || fail "a missing file is said"
[ ! -e "${MELODYD_CONFIG}" ] || fail "and not made"

# A setting starts the file from the skeleton, below its example, private.
"${cli}" config agent-stream-away opus 96 2>/dev/null
grep -q "^# melodyd.conf -- the settings of a Melody engine" "${MELODYD_CONFIG}" ||
    fail "a new file is the skeleton"
grep -A1 -x "#agent-stream-away = opus 128" "${MELODYD_CONFIG}" | grep -qx "agent-stream-away = opus 96" ||
    fail "the setting is below its example"
[ "$(stat -c %a "${MELODYD_CONFIG}")" = 600 ] || fail "the file is its owner's only"
[ "$(stat -c %a "$(dirname "${MELODYD_CONFIG}")")" = 700 ] || fail "and so is its folder"
[ "$("${cli}" config agent-stream-away)" = "opus 96" ] || fail "a setting is read back"

# Changed in place; a section made; a password not echoed.
"${cli}" config agent-stream-away original 2>/dev/null
[ "$(grep -c "^agent-stream-away" "${MELODYD_CONFIG}")" = 1 ] || fail "changed where it is"
"${cli}" config --engine gemenon address 192.168.0.200:6603 2>/dev/null
grep -A1 -x "\[engine gemenon\]" "${MELODYD_CONFIG}" | grep -qx "address = 192.168.0.200:6603" ||
    fail "an [engine] section is made"
[ "$("${cli}" config --engine gemenon address)" = "192.168.0.200:6603" ] || fail "and read"
"${cli}" config password "a long sentence" 2>"${work}/said"
grep -q "password = (set)" "${work}/said" || fail "a password set is not echoed"

# Refused as melodyd would, the file untouched.
cp "${MELODYD_CONFIG}" "${work}/before"
! "${cli}" config agent-stream-home flac 2>"${work}/said" || fail "a bad value is refused"
grep -q "agent-stream-home is original, or opus" "${work}/said" || fail "and why is said"
! "${cli}" config agent-bitrate-away 96 2>"${work}/said" || fail "an old key is refused"
grep -q "it is agent-stream-away now" "${work}/said" || fail "with its new name"
! "${cli}" config --engine gemenon name x 2>/dev/null || fail "a key an [engine] has not is refused"
cmp -s "${work}/before" "${MELODYD_CONFIG}" || fail "a refusal leaves the file as it was"

# Unset; a key not set is said.
"${cli}" config --unset agent-stream-away 2>/dev/null
! "${cli}" config agent-stream-away 2>/dev/null || fail "an unset key is not set"
grep -qx "#agent-stream-away = opus 128" "${MELODYD_CONFIG}" || fail "its example stays"

# --edit: taken when melodyd would take it, refused and kept otherwise.
cat >"${work}/rename.sh" <<'SH'
#!/bin/sh
sed -i 's/^#name = gemenon$/name = caprica/' "$1"
SH
cat >"${work}/break.sh" <<'SH'
#!/bin/sh
printf 'not a setting\n' >>"$1"
SH
chmod +x "${work}/rename.sh" "${work}/break.sh"
EDITOR="${work}/rename.sh" "${cli}" config --edit </dev/null 2>/dev/null
[ "$("${cli}" config name)" = caprica ] || fail "an edit melodyd takes is written"
cp "${MELODYD_CONFIG}" "${work}/before"
! printf 'n\n' | EDITOR="${work}/break.sh" "${cli}" config --edit 2>"${work}/said" ||
    fail "an edit melodyd would refuse is refused"
grep -q "not a setting" "${work}/said" || fail "with the line"
cmp -s "${work}/before" "${MELODYD_CONFIG}" || fail "and the file is as it was"
EDITOR=true "${cli}" config --edit </dev/null 2>"${work}/said"
grep -q "is unchanged" "${work}/said" || fail "an edit that changes nothing says so"
[ -z "$(find "$(dirname "${MELODYD_CONFIG}")" -name '.melodyd.conf.*')" ] || fail "no draft is left"

# Through a symlink, as a dotfile manager makes them: the link stays.
mkdir -p "${work}/dotfiles"
mv "${MELODYD_CONFIG}" "${work}/dotfiles/melodyd.conf"
ln -s "${work}/dotfiles/melodyd.conf" "${MELODYD_CONFIG}"
"${cli}" config upnp yes 2>/dev/null
[ -L "${MELODYD_CONFIG}" ] || fail "a symlinked file stays a link"
grep -qx "upnp = yes" "${work}/dotfiles/melodyd.conf" || fail "and its target is written"

echo "melody-cli config: ok"

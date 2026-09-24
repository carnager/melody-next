#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# What a release carries, built here: the engine, the agent and the CLI for
# this machine's architecture, and the Android app. The release workflow
# (.github/workflows/release.yml) builds the same on a tag, arm64 included.
#
#   scripts/release.sh [VERSION]      # default: from git, e.g. 0.1.0-12-gabc1234
#
# Needs Docker and the Android SDK. The binaries are built on Debian 13 and
# linked against its libraries, so they are Debian 13 binaries; Arch Linux
# has packaging/arch, other distributions the Docker image or a source build. The APK is signed
# when the release key is found (see android/app/build.gradle.kts), and a
# debug build otherwise. Everything lands in dist/VERSION/.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"

version="${1:-$(git describe --tags --always --dirty | sed 's/^v//')}"
code="$(git rev-list --count HEAD)"
arch="$(uname -m)"
case "${arch}" in
    x86_64) arch=amd64 ;;
    aarch64) arch=arm64 ;;
esac
out="${root}/dist/${version}"
mkdir -p "${out}"

say() { printf '==> %s\n' "$*"; }
fail() { printf 'release: %s\n' "$*" >&2; exit 1; }

docker info >/dev/null 2>&1 ||
    fail "Docker is not reachable (not running, or this user is not in the docker group)"

# The binaries: the Dockerfile's binaries stage, copied out of a container
# made from it. buildx could write them straight out; this works without it.
say "Linux binaries for ${arch}"
image="melody-release-binaries:${version}"
docker build --quiet -f packaging/docker/Dockerfile --target binaries -t "${image}" . >/dev/null
container="$(docker create "${image}" /nothing)"
trap 'docker rm -f "${container}" >/dev/null 2>&1 || true' EXIT
staging="$(mktemp -d)"
docker cp "${container}:/" "${staging}/usr"
name="melody-${version}-debian13-${arch}"
mkdir -p "${staging}/${name}"
cp "${staging}"/usr/bin/* "${staging}/${name}/"
cp "${staging}"/usr/lib/systemd/user/*.service "${staging}/${name}/"
cp LICENSE "${staging}/${name}/"
tar -C "${staging}" -czf "${out}/${name}.tar.gz" "${name}"
rm -rf "${staging}"
docker rm -f "${container}" >/dev/null
docker rmi "${image}" >/dev/null
trap - EXIT

# The app: signed when the key is there, as the release workflow does it.
say "Android app ${version} (${code})"
properties=(-PmelodyVersionName="${version}" -PmelodyVersionCode="${code}")
signing="${MELODY_ANDROID_SIGNING_PROPERTIES:-${HOME}/.local/android/release-keys/melody.properties}"
(
    cd android
    if [ -n "${MELODY_ANDROID_STORE_FILE:-}" ] || [ -f "${signing}" ]; then
        ./gradlew --quiet assembleRelease "${properties[@]}"
        cp app/build/outputs/apk/release/app-release.apk "${out}/melody-${version}.apk"
    else
        say "no release key: a debug build, which a signed one cannot update"
        ./gradlew --quiet assembleDebug "${properties[@]}"
        cp app/build/outputs/apk/debug/app-debug.apk "${out}/melody-${version}-debug.apk"
    fi
)

say "done: ${out}"
ls -l "${out}"

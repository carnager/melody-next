#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

preset=dev
jobs=${TRACKKNIFE_BUILD_JOBS:-8}
update=true

usage() {
    cat <<'EOF'
Usage: scripts/build-latest.sh [options]

Fast-forward the current branch from its configured upstream, configure its
CMake preset, and build it.

Options:
  --preset NAME  CMake configure/build preset (default: dev)
  --jobs N       Parallel build jobs (default: 8)
  --no-update    Build the checkout as-is without fetching or pulling
  -h, --help     Show this help

TRACKKNIFE_BUILD_JOBS may also set the default job count.
EOF
}

while (($# > 0)); do
    case $1 in
    --preset)
        [[ $# -ge 2 ]] || { printf '%s\n' 'missing value for --preset' >&2; exit 2; }
        preset=$2
        shift 2
        ;;
    --jobs)
        [[ $# -ge 2 ]] || { printf '%s\n' 'missing value for --jobs' >&2; exit 2; }
        jobs=$2
        shift 2
        ;;
    --no-update)
        update=false
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

[[ $jobs =~ ^[1-9][0-9]*$ ]] || {
    printf 'jobs must be a positive integer: %s\n' "$jobs" >&2
    exit 2
}

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_directory/.." && pwd)
cd "$project_root"

if [[ $update == true ]]; then
    if [[ -n $(git status --porcelain) ]]; then
        printf '%s\n' \
            'The checkout has local changes. Commit or stash them, or use --no-update.' >&2
        exit 1
    fi
    if ! git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' >/dev/null 2>&1; then
        printf '%s\n' 'The current branch has no configured upstream.' >&2
        exit 1
    fi
    git pull --ff-only
fi

configure_arguments=()
if [[ $(uname -s) == Darwin ]] && command -v brew >/dev/null 2>&1; then
    homebrew_prefix=$(brew --prefix)
    qt_prefix=$(brew --prefix qt)
    openssl_prefix=$(brew --prefix openssl@3)
    curl_prefix=$(brew --prefix curl)
    export CMAKE_PREFIX_PATH="$qt_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export PKG_CONFIG_PATH="$homebrew_prefix/lib/pkgconfig:$homebrew_prefix/share/pkgconfig:$openssl_prefix/lib/pkgconfig:$curl_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    configure_arguments+=("-DPKG_CONFIG_EXECUTABLE=$homebrew_prefix/bin/pkg-config")
fi

cmake --preset "$preset" "${configure_arguments[@]}"
cmake --build --preset "$preset" --parallel "$jobs"

application="$project_root/build/$preset/src/bench/trackknife"
if [[ -x $application ]]; then
    printf 'Built %s\n' "$application"
else
    printf 'Build preset %s completed.\n' "$preset"
fi

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

desktop_preset=macos
server_preset=server
server_host=${MELODY_SERVER_HOST:-192.168.1.111}
server_path=${MELODY_SERVER_PATH:-/Users/zeltak/dev/melody-next}
jobs=${TRACKKNIFE_BUILD_JOBS:-8}
update=true
build_server=true
deployment_branch=main
upstream_remote=origin
fork_remote=fork

usage() {
    cat <<'EOF'
Usage: scripts/build-latest.sh [options]

Fast-forward this checkout from the fork's main branch, merge the latest
original melody-next main branch, build Trackknife and Melody on this desktop,
push the fork's main branch, then update and build Melody only on the server.

Options:
  --desktop-preset NAME  Desktop CMake preset (default: macos)
  --server-preset NAME   Server CMake preset (default: server)
  --server HOST          SSH server (default: 192.168.1.111)
  --server-path PATH     Repository on the server
  --jobs N               Parallel build jobs (default: 8)
  --no-update            Do not fetch, merge, or push repository changes
  --desktop-only         Do not update or build the server
  -h, --help             Show this help

Environment overrides: MELODY_SERVER_HOST, MELODY_SERVER_PATH, and
TRACKKNIFE_BUILD_JOBS.
EOF
}

need_value() {
    [[ $# -ge 2 ]] || {
        printf 'missing value for %s\n' "$1" >&2
        exit 2
    }
}

while (($# > 0)); do
    case $1 in
    --desktop-preset)
        need_value "$@"
        desktop_preset=$2
        shift 2
        ;;
    --server-preset)
        need_value "$@"
        server_preset=$2
        shift 2
        ;;
    --server)
        need_value "$@"
        server_host=$2
        shift 2
        ;;
    --server-path)
        need_value "$@"
        server_path=$2
        shift 2
        ;;
    --jobs)
        need_value "$@"
        jobs=$2
        shift 2
        ;;
    --no-update)
        update=false
        shift
        ;;
    --desktop-only)
        build_server=false
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
            'The desktop checkout has local changes. Commit or stash them, or use --no-update.' >&2
        exit 1
    fi

    git fetch "$upstream_remote" "$deployment_branch"
    git fetch "$fork_remote" "$deployment_branch"
    if [[ $(git branch --show-current) != "$deployment_branch" ]]; then
        git switch "$deployment_branch"
    fi
    git merge --ff-only "$fork_remote/$deployment_branch"
    if ! git merge-base --is-ancestor \
        "$upstream_remote/$deployment_branch" HEAD; then
        git merge --no-edit "$upstream_remote/$deployment_branch"
    fi
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

cmake --preset "$desktop_preset" "${configure_arguments[@]}"
cmake --build --preset "$desktop_preset" --target trackknife melodyd --parallel "$jobs"
printf 'Built desktop Trackknife and Melody in %s/build/%s\n' "$project_root" "$desktop_preset"

if [[ $update == true ]]; then
    git push "$fork_remote" "$deployment_branch"
fi

if [[ $build_server == false ]]; then
    exit 0
fi

ssh "$server_host" bash -s -- \
    "$server_path" "$server_preset" "$jobs" "$update" "$deployment_branch" <<'REMOTE'
set -euo pipefail

project_root=$1
preset=$2
jobs=$3
update=$4
deployment_branch=$5
cd "$project_root"

if [[ $update == true ]]; then
    if [[ -n $(git status --porcelain) ]]; then
        printf '%s\n' 'The server checkout has local changes; refusing to overwrite them.' >&2
        exit 1
    fi
    git fetch origin "$deployment_branch"
    if git show-ref --verify --quiet "refs/heads/$deployment_branch"; then
        git switch "$deployment_branch"
    else
        git switch --track -c "$deployment_branch" "origin/$deployment_branch"
    fi
    git merge --ff-only "origin/$deployment_branch"
fi

configure_arguments=()
if [[ $(uname -s) == Darwin ]] && [[ -x /opt/homebrew/bin/brew ]]; then
    homebrew_prefix=$(/opt/homebrew/bin/brew --prefix)
    openssl_prefix=$(/opt/homebrew/bin/brew --prefix openssl@3)
    curl_prefix=$(/opt/homebrew/bin/brew --prefix curl)
    export PATH="$homebrew_prefix/bin:$PATH"
    export PKG_CONFIG_PATH="$homebrew_prefix/lib/pkgconfig:$homebrew_prefix/share/pkgconfig:$openssl_prefix/lib/pkgconfig:$curl_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    configure_arguments+=("-DPKG_CONFIG_EXECUTABLE=$homebrew_prefix/bin/pkg-config")
fi

cmake --preset "$preset" "${configure_arguments[@]}"
cmake --build --preset "$preset" --target melodyd --parallel "$jobs"
printf 'Built server Melody at %s/build/%s/src/daemon/melodyd\n' "$project_root" "$preset"
REMOTE

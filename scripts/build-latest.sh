#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

desktop_preset=macos
server_preset=server
server_host=${MELODY_SERVER_HOST:-192.168.1.111}
server_path=${MELODY_SERVER_PATH:-/Users/zeltak/dev/melody-next}
server_install_dir=${MELODY_SERVER_INSTALL_DIR:-melody}
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
push the fork's main branch, then copy the Release Melody binary to the server.

Options:
  --desktop-preset NAME  Desktop CMake preset (default: macos)
  --server-preset NAME   Server CMake preset (default: server)
  --server HOST          SSH server (default: 192.168.1.111)
  --server-path PATH     Repository on the server
  --server-install-dir PATH
                         Install directory relative to the server home
                         (default: melody, meaning ~/melody)
  --jobs N               Parallel build jobs (default: 8)
  --no-update            Do not fetch, merge, or push repository changes
  --desktop-only         Do not update or build the server
  -h, --help             Show this help

Environment overrides: MELODY_SERVER_HOST, MELODY_SERVER_PATH,
MELODY_SERVER_INSTALL_DIR, and TRACKKNIFE_BUILD_JOBS.
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
    --server-install-dir)
        need_value "$@"
        server_install_dir=$2
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

if [[ $build_server == true ]]; then
    cmake --preset "$server_preset" "${configure_arguments[@]}"
    cmake --build --preset "$server_preset" --target melodyd --parallel "$jobs"
    server_binary="$project_root/build/$server_preset/src/daemon/melodyd"
    [[ -x $server_binary ]] || {
        printf 'Server binary was not produced at %s\n' "$server_binary" >&2
        exit 1
    }
    printf 'Built server Melody binary at %s\n' "$server_binary"
fi

if [[ $update == true ]]; then
    git push "$fork_remote" "$deployment_branch"
fi

if [[ $build_server == false ]]; then
    exit 0
fi

ssh "$server_host" bash -s -- \
    "$server_path" "$update" "$deployment_branch" "$server_install_dir" <<'REMOTE'
set -euo pipefail

project_root=$1
update=$2
deployment_branch=$3
install_dir=$4
cd "$project_root"

if [[ $update == true ]]; then
    if [[ -n $(git status --porcelain) ]]; then
        printf '%s\n' 'The server checkout has local changes; refusing to overwrite them.' >&2
        exit 1
    fi
    git fetch origin "$deployment_branch"
    if git show-ref --verify --quiet "refs/heads/$deployment_branch"; then
        git switch "$deployment_branch"
        if ! git merge-base --is-ancestor \
            "$deployment_branch" "origin/$deployment_branch"; then
            backup_branch="backup/server-${deployment_branch}-before-sync-$(date +%Y%m%d-%H%M%S)"
            git branch "$backup_branch" "$deployment_branch"
            printf 'Preserved diverged server branch as %s\n' "$backup_branch"
            git reset --hard "origin/$deployment_branch"
        else
            git merge --ff-only "origin/$deployment_branch"
        fi
    else
        git switch --track -c "$deployment_branch" "origin/$deployment_branch"
    fi
fi

case $install_dir in
/*) install_path=$install_dir ;;
*) install_path="$HOME/$install_dir" ;;
esac
/bin/mkdir -p "$install_path"
REMOTE

deployment_commit=$(git rev-parse HEAD)
server_checksum=$(/usr/bin/shasum -a 256 "$server_binary" | /usr/bin/awk '{print $1}')
upload_name=".melodyd-upload-$$"
scp -p "$server_binary" "$server_host:$server_install_dir/$upload_name"

ssh "$server_host" bash -s -- \
    "$server_install_dir" "$upload_name" "$server_checksum" "$deployment_commit" <<'REMOTE'
set -euo pipefail

install_dir=$1
upload_name=$2
expected_checksum=$3
deployment_commit=$4
case $install_dir in
/*) install_path=$install_dir ;;
*) install_path="$HOME/$install_dir" ;;
esac
uploaded_binary="$install_path/$upload_name"
installed_binary="$install_path/melodyd"
actual_checksum=$(/usr/bin/shasum -a 256 "$uploaded_binary" | /usr/bin/awk '{print $1}')
if [[ $actual_checksum != "$expected_checksum" ]]; then
    printf 'Checksum mismatch after SCP: expected %s, got %s\n' \
        "$expected_checksum" "$actual_checksum" >&2
    exit 1
fi

/bin/chmod 755 "$uploaded_binary"
/bin/mv -f "$uploaded_binary" "$installed_binary"
installed_size=$(/usr/bin/stat -f '%z' "$installed_binary")

printf '\n'
printf '%s\n' '========== SERVER DEPLOYMENT SUCCESS =========='
printf 'Host:       %s\n' "$(/bin/hostname)"
printf 'Binary:     %s\n' "$installed_binary"
printf 'Commit:     %s\n' "$deployment_commit"
printf 'Size:       %s bytes\n' "$installed_size"
printf 'SHA-256:    %s\n' "$actual_checksum"
printf '%s\n' '================================================'
REMOTE

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

desktop_preset=macos
server_preset=macos-server
server_host=${MELODY_SERVER_HOST:-192.168.1.111}
server_path=${MELODY_SERVER_PATH:-/Users/zeltak/dev/melody-next}
server_install_dir=${MELODY_SERVER_INSTALL_DIR:-melody}
fork_remote=${MELODY_FORK_REMOTE:-fork}
jobs=${TRACKKNIFE_BUILD_JOBS:-8}
build_server=true

usage() {
    cat <<'EOF'
Usage: scripts/build-current-branch.sh [options]

Build the current committed branch without fetching or merging main. After the
desktop build succeeds, push that branch to the fork and build the exact same
commit in a temporary worktree on the server. The server checkout itself is
left on its existing branch.

Options:
  --desktop-preset NAME  Desktop CMake preset (default: macos)
  --server-preset NAME   Server CMake preset (default: macos-server)
  --server HOST          SSH server (default: 192.168.1.111)
  --server-path PATH     Repository on the server
  --server-install-dir PATH
                         Install directory relative to the server home
                         (default: melody, meaning ~/melody)
  --fork-remote NAME     Local Git remote for the fork (default: fork)
  --jobs N               Parallel build jobs (default: 8)
  --desktop-only         Build only the local desktop; do not push or deploy
  -h, --help             Show this help

Environment overrides: MELODY_SERVER_HOST, MELODY_SERVER_PATH,
MELODY_SERVER_INSTALL_DIR, MELODY_FORK_REMOTE, and TRACKKNIFE_BUILD_JOBS.
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
    --fork-remote)
        need_value "$@"
        fork_remote=$2
        shift 2
        ;;
    --jobs)
        need_value "$@"
        jobs=$2
        shift 2
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

branch=$(git branch --show-current)
[[ -n $branch ]] || {
    printf '%s\n' 'The desktop checkout is detached; switch to the branch you want to test.' >&2
    exit 1
}
if [[ -n $(git status --porcelain) ]]; then
    printf '%s\n' \
        'The desktop checkout has local changes. Commit them so the server can build the same state.' >&2
    exit 1
fi
commit=$(git rev-parse HEAD)

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

printf 'Building desktop branch %s at %s\n' "$branch" "$commit"
cmake --preset "$desktop_preset" "${configure_arguments[@]}"
cmake --build --preset "$desktop_preset" --target trackknife melodyd --parallel "$jobs"
printf 'Built desktop Trackknife and Melody from %s at %s\n' "$branch" "$commit"

if [[ $build_server == false ]]; then
    exit 0
fi

fork_url=$(git remote get-url "$fork_remote")
git push "$fork_remote" "HEAD:refs/heads/$branch"
printf 'Pushed %s at %s to %s\n' "$branch" "$commit" "$fork_remote"

ssh "$server_host" bash -s -- \
    "$server_path" "$server_preset" "$jobs" "$branch" "$commit" \
    "$fork_url" "$server_install_dir" <<'REMOTE'
set -euo pipefail

project_root=$1
preset=$2
jobs=$3
branch=$4
expected_commit=$5
fork_url=$6
install_dir=$7

git -C "$project_root" fetch --no-tags "$fork_url" "refs/heads/$branch"
fetched_commit=$(git -C "$project_root" rev-parse FETCH_HEAD)
if [[ $fetched_commit != "$expected_commit" ]]; then
    printf 'Fetched commit mismatch: expected %s, got %s\n' \
        "$expected_commit" "$fetched_commit" >&2
    exit 1
fi

worktree_path=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/melody-branch-build.XXXXXX")
/bin/rmdir "$worktree_path"
cleanup() {
    git -C "$project_root" worktree remove --force "$worktree_path" >/dev/null 2>&1 || true
}
trap cleanup EXIT
git -C "$project_root" worktree add --detach "$worktree_path" "$expected_commit"

case $install_dir in
/*) install_path=$install_dir ;;
*) install_path="$HOME/$install_dir" ;;
esac
/bin/mkdir -p "$install_path"

configure_arguments=()
if [[ $(/usr/bin/uname -s) == Darwin ]] && [[ -x /opt/homebrew/bin/brew ]]; then
    homebrew_prefix=$(/opt/homebrew/bin/brew --prefix)
    openssl_prefix=$(/opt/homebrew/bin/brew --prefix openssl@3)
    curl_prefix=$(/opt/homebrew/bin/brew --prefix curl)
    export PATH="$homebrew_prefix/bin:$PATH"
    export PKG_CONFIG_PATH="$homebrew_prefix/lib/pkgconfig:$homebrew_prefix/share/pkgconfig:$openssl_prefix/lib/pkgconfig:$curl_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    configure_arguments+=("-DPKG_CONFIG_EXECUTABLE=$homebrew_prefix/bin/pkg-config")
fi

cmake --preset "$preset" -S "$worktree_path" "${configure_arguments[@]}"
cmake --build "$worktree_path/build/$preset" --target melodyd --parallel "$jobs"
server_binary="$worktree_path/build/$preset/src/daemon/melodyd"
[[ -x $server_binary ]] || {
    printf 'Server binary was not produced at %s\n' "$server_binary" >&2
    exit 1
}

expected_checksum=$(/usr/bin/shasum -a 256 "$server_binary" | /usr/bin/awk '{print $1}')
staged_binary="$install_path/.melodyd-install-$$"
installed_binary="$install_path/melodyd"
/bin/cp -p "$server_binary" "$staged_binary"
actual_checksum=$(/usr/bin/shasum -a 256 "$staged_binary" | /usr/bin/awk '{print $1}')
if [[ $actual_checksum != "$expected_checksum" ]]; then
    printf 'Checksum mismatch while installing: expected %s, got %s\n' \
        "$expected_checksum" "$actual_checksum" >&2
    exit 1
fi

/bin/chmod 755 "$staged_binary"
/bin/mv -f "$staged_binary" "$installed_binary"
"$installed_binary" --help >/dev/null 2>&1
installed_size=$(/usr/bin/stat -f '%z' "$installed_binary")

printf '\n'
printf '%s\n' '======= CURRENT BRANCH DEPLOYMENT SUCCESS ======='
printf 'Host:       %s\n' "$(/bin/hostname)"
printf 'Branch:     %s\n' "$branch"
printf 'Commit:     %s\n' "$expected_commit"
printf 'Binary:     %s\n' "$installed_binary"
printf 'Size:       %s bytes\n' "$installed_size"
printf 'SHA-256:    %s\n' "$actual_checksum"
printf '%s\n' 'Launch test: passed (melodyd --help)'
printf '%s\n' 'Server checkout: unchanged (temporary worktree removed)'
printf '%s\n' '=================================================='
REMOTE

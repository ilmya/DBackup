#!/usr/bin/env bash
set -euo pipefail

test_home="$(mktemp -d "${TMPDIR:-/tmp}/dbackup-keyring-test-XXXXXX")"
cleanup() { rm -rf -- "$test_home"; }
trap cleanup EXIT
export HOME="$test_home"
export XDG_DATA_HOME="$test_home/.local/share"
export XDG_CONFIG_HOME="$test_home/.config"
export XDG_RUNTIME_DIR="$test_home/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY

dbus-run-session -- bash -c '
    set -euo pipefail
    printf "%s\n" "dbackup-temporary-test-keyring" | gnome-keyring-daemon --unlock --components=secrets >/dev/null
    exec "$@"
' bash "$@"

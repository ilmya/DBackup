#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${DBACKUP_BUILD_DIR:-${root}/out/build-test}"
qt_prefix="${QT_ROOT:-${HOME}/Qt/6.8.3/gcc_64}"
command_name="${1:-build}"

case "$command_name" in
  build|test|install)
    cmake -S "$root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_prefix" \
      -DBUILD_TESTS=ON -DDBACKUP_BUILD_TEST_CLI=ON
    cmake --build "$build_dir" --parallel "${DBACKUP_JOBS:-2}"
    if [[ "$command_name" == test ]]; then
      ctest --test-dir "$build_dir" --output-on-failure
    elif [[ "$command_name" == install ]]; then
      cmake --install "$build_dir" --prefix "${DBACKUP_INSTALL_PREFIX:-${HOME}/.local}"
    fi
    ;;
  *) printf 'Usage: bash build.sh [build|test|install]\n' >&2; exit 2 ;;
esac

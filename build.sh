#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${DBACKUP_BUILD_DIR:-${root}/out/build-test}"
qt_prefix="${QT_ROOT:-${HOME}/Qt/6.8.3/gcc_64}"
command_name="${1:-run}"

usage() {
  cat <<'EOF'
用法: bash build.sh [run|build|test|install|help]
  run      构建并启动图形界面（默认）
  build    仅构建
  test     构建并在隔离密钥环中运行测试
  install  构建并安装（默认安装至 ~/.local）
环境变量: QT_ROOT、DBACKUP_BUILD_DIR、DBACKUP_INSTALL_PREFIX、DBACKUP_JOBS
EOF
}

if (( $# > 1 )); then usage >&2; exit 2; fi

case "$command_name" in
  help|-h|--help) usage; exit 0 ;;
  run|build|test|install)
    for tool in cmake ninja pkg-config; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        printf '缺少必要工具: %s；请参阅 README 安装依赖。\n' "$tool" >&2
        exit 1
      fi
    done
    if [[ ! -d "$qt_prefix" ]]; then
      printf '找不到 Qt 目录: %s；请设置 QT_ROOT。\n' "$qt_prefix" >&2
      exit 1
    fi
    if [[ "$command_name" == test ]]; then
      for tool in dbus-run-session gnome-keyring-daemon; do
        if ! command -v "$tool" >/dev/null 2>&1; then
          printf '测试需要 %s；请参阅 README 安装测试依赖。\n' "$tool" >&2
          exit 1
        fi
      done
    fi
    cmake -S "$root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_prefix" \
      -DBUILD_TESTS=ON -DDBACKUP_BUILD_TEST_CLI=ON
    cmake --build "$build_dir" --parallel "${DBACKUP_JOBS:-2}"
    if [[ "$command_name" == test ]]; then
      bash "$root/tests/run_in_test_keyring.sh" ctest --test-dir "$build_dir" --output-on-failure
    elif [[ "$command_name" == install ]]; then
      cmake --install "$build_dir" --prefix "${DBACKUP_INSTALL_PREFIX:-${HOME}/.local}"
    elif [[ "$command_name" == run ]]; then
      exec "$build_dir/BackupTool"
    fi
    ;;
  *) usage >&2; exit 2 ;;
esac

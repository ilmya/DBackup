#!/usr/bin/env bash
# Linux 构建、测试、覆盖率和程序启动脚本
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${DBACKUP_BUILD_DIR:-${root}/out/build-test}"
coverage_build_dir="${DBACKUP_COVERAGE_BUILD_DIR:-${root}/out/build-coverage}"
qt_prefix="${QT_ROOT:-${HOME}/Qt/6.8.3/gcc_64}"
command_name="${1:-run}"

usage() {
  cat <<'EOF'
用法: bash build.sh [run|root|build|test|coverage|help]
  run         配置、增量构建并启动图形界面（默认）
  root        以当前用户构建，然后以 root 启动图形界面
  build       仅构建
  test        构建并运行全部单元、集成和支持工具测试
  coverage    在独立 GCC 构建目录运行单元测试并生成 gcov 摘要
  help        显示此帮助
环境变量: QT_ROOT、DBACKUP_BUILD_DIR、DBACKUP_COVERAGE_BUILD_DIR、DBACKUP_JOBS
测试隔离目录: DBACKUP_TEST_DATA_ROOT、DBACKUP_TEST_TEMP_ROOT、DBACKUP_TEST_TMP_ROOT、DBACKUP_TEST_RUNTIME_ROOT
EOF
}

if (( $# > 1 )); then usage >&2; exit 2; fi

case "$command_name" in
  help|-h|--help) usage; exit 0 ;;
  run|root|build|test|coverage) ;;
  *) usage >&2; exit 2 ;;
esac

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

is_test_command=false
case "$command_name" in
  test|coverage) is_test_command=true ;;
esac

if [[ "$is_test_command" == true ]]; then
  for tool in python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      printf '测试需要 %s；请参阅 README 安装测试依赖。\n' "$tool" >&2
      exit 1
    fi
  done
  if [[ "$command_name" == test ]]; then
    for tool in dbus-run-session gnome-keyring-daemon; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        printf '集成测试需要 %s；请参阅 README 安装测试依赖。\n' "$tool" >&2
        exit 1
      fi
    done
  fi
  if [[ "$command_name" == coverage ]]; then
    for tool in g++ gcov; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        printf '覆盖率报告需要 GCC 工具 %s。\n' "$tool" >&2
        exit 1
      fi
    done
    test_build_dir="$coverage_build_dir"
  else
    test_build_dir="$build_dir"
  fi
  if [[ -n "${DBACKUP_TEST_TMP_ROOT:-}" ]]; then
    test_tmp_root="$DBACKUP_TEST_TMP_ROOT"
  else
    filesystem_type="$(stat -f -c %T "$root" 2>/dev/null || true)"
    case "$filesystem_type" in
      v9fs|drvfs)
        automatic_test_tmp_root="$(mktemp -d /tmp/dbackup-build-test-XXXXXX)"
        trap 'rm -rf -- "$automatic_test_tmp_root"' EXIT
        test_tmp_root="$automatic_test_tmp_root"
        ;;
      *) test_tmp_root="$test_build_dir/test-tmp" ;;
    esac
  fi
  if [[ -L "$test_tmp_root" ]]; then
    printf '测试临时目录不能是符号链接: %s\n' "$test_tmp_root" >&2
    exit 1
  fi
  mkdir -p "$test_tmp_root"
  export TMPDIR="$test_tmp_root"
  export DBACKUP_TEST_DATA_ROOT="${DBACKUP_TEST_DATA_ROOT:-$test_build_dir/acceptance-data}"
  export DBACKUP_TEST_TEMP_ROOT="${DBACKUP_TEST_TEMP_ROOT:-$test_build_dir/acceptance-selftest-temp}"
fi

configure_project() {
  local target_build_dir="$1"
  local build_type="$2"
  shift 2
  local cmake_options=("$@")
  local build_tests=OFF
  if [[ "$is_test_command" == true ]]; then build_tests=ON; fi
  if [[ -n "${DBACKUP_TEST_DATA_ROOT:-}" ]]; then cmake_options+=("-DDBACKUP_TEST_DATA_ROOT=$DBACKUP_TEST_DATA_ROOT"); fi
  if [[ -n "${DBACKUP_TEST_TEMP_ROOT:-}" ]]; then cmake_options+=("-DDBACKUP_TEST_TEMP_ROOT=$DBACKUP_TEST_TEMP_ROOT"); fi
  cmake -S "$root" -B "$target_build_dir" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$build_type" "-DCMAKE_PREFIX_PATH=$qt_prefix" \
    "-DQt6_DIR=$qt_prefix/lib/cmake/Qt6" \
    "-DBUILD_TESTS=$build_tests" "-DDBACKUP_BUILD_TEST_CLI=$build_tests" "${cmake_options[@]}"
}

run_ctest_group() {
  local target_build_dir="$1"
  local group="$2"
  local ctest_args=(--test-dir "$target_build_dir" --output-on-failure --label-summary)
  case "$group" in
    all) ;;
    unit) ctest_args+=(-L unit) ;;
    *) printf '内部错误：未知测试组 %s\n' "$group" >&2; return 2 ;;
  esac
  if [[ "$group" == unit ]]; then
    export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/dbackup-unit-test-bus
    ctest "${ctest_args[@]}"
  else
    bash "$root/tests/run_in_test_keyring.sh" ctest "${ctest_args[@]}"
  fi
}

clear_coverage_counters() {
  [[ -d "$coverage_build_dir" ]] || return 0
  local project_path coverage_path
  project_path="$(realpath -m -- "$root")"
  coverage_path="$(realpath -m -- "$coverage_build_dir")"
  case "$coverage_path" in
    "$project_path"/*)
      find "$coverage_path" -type f -name '*.gcda' -delete
      ;;
    *)
      printf '跳过项目外覆盖率目录中的旧计数文件；如需全新结果，请使用新的 DBACKUP_COVERAGE_BUILD_DIR。\n' >&2
      ;;
  esac
}

case "$command_name" in
  coverage)
    clear_coverage_counters
    configure_project "$coverage_build_dir" Debug \
      -DCMAKE_CXX_COMPILER=g++ \
      "-DCMAKE_CXX_FLAGS=--coverage -O0 -g" \
      "-DCMAKE_EXE_LINKER_FLAGS=--coverage"
    cmake --build "$coverage_build_dir" --parallel "${DBACKUP_JOBS:-2}" \
      --target BackupToolTests BackupToolAutomationTests
    run_ctest_group "$coverage_build_dir" unit
    python3 -B "$root/tests/report_gcov.py" --build-dir "$coverage_build_dir" \
      --output-dir "$coverage_build_dir/coverage"
    ;;
  *)
    configure_project "$build_dir" Release
    cmake --build "$build_dir" --parallel "${DBACKUP_JOBS:-2}"
    case "$command_name" in
      test) run_ctest_group "$build_dir" all ;;
      run) exec "$build_dir/BackupTool" ;;
      root)
        gui_env=()
        for name in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR; do
          if [[ -v "$name" ]]; then gui_env+=("$name=${!name}"); fi
        done
        exec sudo env "${gui_env[@]}" "$build_dir/BackupTool"
        ;;
    esac
    ;;
esac

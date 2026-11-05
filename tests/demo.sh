#!/usr/bin/env bash
set -euo pipefail
project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
command_name="${1:-run}"
case "$command_name" in
  prepare) exec python3 "$project/tests/demo.py" prepare ;;
  run)
    python3 "$project/tests/demo.py" prepare
    bash "$project/build.sh" build
    exec python3 "$project/tests/demo.py" run --cli "${DBACKUP_BUILD_DIR:-$project/out/build-test}/BackupToolTestCli"
    ;;
  scenarios|privileged)
    python3 "$project/tests/demo.py" prepare
    bash "$project/build.sh" build
    cli="${DBACKUP_BUILD_DIR:-$project/out/build-test}/BackupToolTestCli"
    if [[ "$command_name" == privileged ]]; then
      demo_root="${DBACKUP_DEMO_ROOT:-$HOME/DBackup-Demo}"
      exec sudo -- python3 "$project/tests/demo.py" scenarios --root "$demo_root" --cli "$cli"
    fi
    exec python3 "$project/tests/demo.py" scenarios --cli "$cli"
    ;;
  compare) shift; exec python3 "$project/tests/demo.py" compare "$@" ;;
  *) printf '用法: bash tests/demo.sh [prepare|run|scenarios|privileged|compare 恢复目录]\n' >&2; exit 2 ;;
esac

#!/usr/bin/env bash
set -euo pipefail
project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
command_name="${1:-help}"
case "$command_name" in
  compare)
    shift
    exec python3 -B "$project/tests/demo.py" compare "$@"
    ;;
  help|-h|--help)
    cat <<'EOF'
用法:
  bash tests/demo.sh compare <filters|large-data|special-files> <恢复目录>

compare 报告写入 DBACKUP_BUILD_DIR/demo-reports。
EOF
    ;;
  *) printf '未知命令：%s\n运行 bash tests/demo.sh help 查看用法。\n' "$command_name" >&2; exit 2 ;;
esac

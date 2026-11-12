#!/usr/bin/env bash
# 清理项目构建产物的脚本
set -eo pipefail

project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 -B - "$project" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + '/tests')
import demo

try:
    root = demo.canonical_root(None)
    count = demo.clear_restore(root)
    print(f'已清空 {root / "restore"}，删除 {count} 个顶层项目。')
except (OSError, ValueError, demo.tool.SafetyError) as exc:
    print(f'清理未完成：{exc}', file=sys.stderr)
    sys.exit(1)
PY

#!/usr/bin/env bash
# 演示和验收验证脚本入口
set -euo pipefail
project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -B "$project/tests/demo_verify.py" "$@"

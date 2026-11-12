#!/usr/bin/env bash
# 执行独立验收场景的入口脚本
set -euo pipefail

# Linux entry point for the independent DBackup acceptance toolkit
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
tool="${script_dir}/dbackup_acceptance.py"

python_cmd=(python3)

export PYTHONUTF8=1
exec "${python_cmd[@]}" "$tool" "$@"

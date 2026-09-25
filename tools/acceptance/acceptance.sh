#!/usr/bin/env bash
set -euo pipefail

# Git Bash entry point for the independent DBackup acceptance toolkit.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -W)"
tool="${script_dir}\\dbackup_acceptance.py"

if command -v py.exe >/dev/null 2>&1; then
  python_cmd=(py.exe -3)
elif command -v python.exe >/dev/null 2>&1; then
  python_cmd=(python.exe)
elif command -v python3 >/dev/null 2>&1; then
  python_cmd=(python3)
else
  echo "错误：未找到 Python 3。请安装 Python 3 并将其加入 PATH。" >&2
  exit 2
fi

export PYTHONUTF8=1
exec "${python_cmd[@]}" "$tool" "$@"

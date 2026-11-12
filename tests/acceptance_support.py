#!/usr/bin/env python3
# 验收场景共用的运行与结果处理工具
"""Extra safe acceptance operations that supplement the independent toolkit."""

import argparse
import fcntl
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "acceptance"))
import dbackup_acceptance as tool  # noqa: E402


def contained(root: Path, value: str) -> Path:
    path = Path(value).resolve()
    if path == root or root not in path.parents:
        raise tool.SafetyError(f"路径必须在验收根目录内: {path}")
    current = path
    while current != root:
        if current.exists() and current.is_symlink():
            raise tool.SafetyError(f"操作路径不能经过符号链接: {current}")
        current = current.parent
    return path


def fault_copy(args) -> int:
    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    source, output = contained(root, args.input), contained(root, args.output)
    if source == output or not source.exists() or output.exists():
        raise tool.SafetyError("来源必须存在，输出必须是不存在的独立路径")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not source.is_file():
        raise ValueError("备份故障类型的输入必须为普通文件")
    shutil.copyfile(source, output)
    content = bytearray(output.read_bytes())
    if args.kind == "archive-empty":
        content.clear()
    elif args.kind == "archive-truncate":
        content = content[:len(content) // 2]
    elif args.kind == "archive-header":
        if content:
            content[0] ^= 0xFF
    elif args.kind == "archive-tag":
        if len(content) < 48:
            output.unlink(missing_ok=True)
            raise ValueError("备份样本过短，无法翻转尾部认证数据")
        content[-1] ^= 0x01
    output.write_bytes(content)
    print(f"故障副本已生成：{output}；来源未更改。")
    return tool.EXIT_OK


def hold_file(args) -> int:
    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    path = contained(root, args.path)
    if not path.is_file():
        return tool.EXIT_BLOCKED
    with path.open("rb") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        print(f"已对测试文件设置 Linux 建议锁：{path}\n按 Enter 释放。")
        print("注意：Linux 建议锁只限制主动使用 flock 的程序。")
        input()
    return tool.EXIT_OK


def realtime_events(args) -> int:
    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    live = contained(root, str(root / "fixtures/realtime/live"))
    live.mkdir(parents=True, exist_ok=True)
    active = live / "burst.tmp"
    active.write_text("event 0\n", encoding="utf-8")
    for index in range(1, 11):
        active.write_text(f"event {index}\n", encoding="utf-8")
        time.sleep(args.interval)
    renamed = live / "renamed.txt"
    active.rename(renamed)
    time.sleep(args.interval)
    (live / "created.txt").write_text("created\n", encoding="utf-8")
    time.sleep(args.interval)
    renamed.unlink()
    print(f"已在 {live} 依次产生连续修改、重命名、新建和删除事件。")
    return tool.EXIT_OK


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    fault = sub.add_parser("fault-copy")
    fault.add_argument("--root", required=True)
    fault.add_argument("--kind", choices=("archive-empty", "archive-truncate", "archive-header", "archive-tag"), required=True)
    fault.add_argument("--input", required=True)
    fault.add_argument("--output", required=True)
    fault.set_defaults(handler=fault_copy)
    hold = sub.add_parser("hold-file")
    hold.add_argument("--root", required=True)
    hold.add_argument("--path", required=True)
    hold.set_defaults(handler=hold_file)
    events = sub.add_parser("realtime-events")
    events.add_argument("--root", required=True)
    events.add_argument("--interval", type=float, default=0.25)
    events.set_defaults(handler=realtime_events)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except tool.SafetyError as exc:
        print(f"SAFETY: {exc}", file=sys.stderr)
        return tool.EXIT_SAFETY
    except PermissionError as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        return tool.EXIT_BLOCKED
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return tool.EXIT_USAGE


if __name__ == "__main__":
    raise SystemExit(main())

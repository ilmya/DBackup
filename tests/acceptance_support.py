#!/usr/bin/env python3
"""Extra safe acceptance operations that supplement the independent toolkit."""

import argparse
import ctypes
from ctypes import wintypes
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
    if args.kind.startswith("repo-"):
        if not source.is_dir():
            raise ValueError("repo-* 故障类型的输入必须为仓库目录")
        shutil.copytree(source, output, symlinks=True)
        chunks = [p for p in (output / "chunks").rglob("*.blk") if p.is_file() and not p.is_symlink()]
        if not chunks:
            raise ValueError("仓库中没有可修改的数据块")
        target = chunks[0]
        if args.kind == "repo-missing":
            target.unlink()
        else:
            content = bytearray(target.read_bytes())
            if not content:
                raise ValueError("数据块为空，不能生成损坏副本")
            content[len(content) // 2] ^= 0x01
            target.write_bytes(content)
    else:
        if not source.is_file():
            raise ValueError("归档故障类型的输入必须为普通文件")
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
                raise ValueError("归档样本过短，无法翻转尾部认证数据")
            content[-1] ^= 0x01
        output.write_bytes(content)
    print(f"故障副本已生成：{output}；来源未更改。")
    return tool.EXIT_OK


def hold_file(args) -> int:
    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    path = contained(root, args.path)
    if not path.is_file() or sys.platform != "win32":
        return tool.EXIT_BLOCKED
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
    if handle == wintypes.HANDLE(-1).value:
        raise PermissionError(ctypes.get_last_error(), "无法独占打开测试文件", str(path))
    print(f"测试文件已独占锁定：{path}\n按 Enter 释放锁定。")
    try:
        input()
    finally:
        kernel.CloseHandle(handle)
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
    fault.add_argument("--kind", choices=("archive-empty", "archive-truncate", "archive-header", "archive-tag", "repo-missing", "repo-corrupt"), required=True)
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

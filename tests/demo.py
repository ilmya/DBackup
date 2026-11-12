#!/usr/bin/env python3
# 生成并验证演示备份场景
"""Compare the fixed WSL manual-acceptance corpus."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import stat
import sys

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools" / "acceptance"))
import dbackup_acceptance as tool  # noqa: E402


DEFAULT_ROOT = Path("/home/ilm/DBackup-Demo")
MARKER = tool.MARKER
LAYOUT_VERSION = 2
TOP_LEVEL = ("common", "large-data", "special-files", "restore")
IGNORED_TOP_LEVEL = {"task", "task-backup"}


class DemoError(RuntimeError):
    pass


def canonical_root(value: str | None) -> Path:
    requested = Path(value or DEFAULT_ROOT).expanduser()
    if not requested.is_absolute():
        raise tool.SafetyError("演示根目录必须是绝对路径")
    candidate = Path(os.path.abspath(requested))
    canonical = DEFAULT_ROOT
    if candidate != canonical or candidate.parent.resolve() / candidate.name != canonical:
        raise tool.SafetyError(f"本脚本仅允许操作固定演示目录：{canonical}")
    if candidate.is_symlink():
        raise tool.SafetyError(f"演示根目录不能是符号链接：{candidate}")
    return candidate


def validate_marker(root: Path, *, require_current: bool) -> dict:
    marker_path = root / MARKER
    try:
        marker_info = marker_path.lstat()
    except FileNotFoundError as exc:
        raise tool.SafetyError(f"演示目录缺少安全标记，拒绝操作：{root}") from exc
    if not stat.S_ISREG(marker_info.st_mode):
        raise tool.SafetyError("安全标记必须是普通文件，不能是链接或其他类型")
    marker = tool.load_marker(root)
    if require_current and marker.get("demoLayoutVersion") != LAYOUT_VERSION:
        raise tool.SafetyError("演示目录标记版本不符，请检查现有样例目录。")
    return marker


def validate_root_directory(root: Path) -> None:
    if root.is_symlink():
        raise tool.SafetyError(f"演示根目录不能是符号链接：{root}")
    try:
        info = root.lstat()
    except FileNotFoundError as exc:
        raise tool.SafetyError(f"演示根目录不存在：{root}") from exc
    if not stat.S_ISDIR(info.st_mode):
        raise tool.SafetyError(f"演示根目录不是目录：{root}")


def validate_layout(root: Path) -> None:
    validate_root_directory(root)
    validate_marker(root, require_current=True)
    expected = {MARKER, *TOP_LEVEL}
    with os.scandir(root) as entries:
        actual = {entry.name for entry in entries}
    missing = sorted(expected - actual)
    extra = sorted(actual - expected - IGNORED_TOP_LEVEL)
    if missing or extra:
        raise tool.SafetyError(
            f"演示目录结构不符（缺少：{missing or '无'}；多出：{extra or '无'}）；"
            "请检查演示目录。"
        )
    for name in TOP_LEVEL:
        path = root / name
        if path.is_symlink() or not path.is_dir():
            raise tool.SafetyError(f"演示子目录缺失、类型不符或是符号链接：{path}")


def clear_restore(root: Path) -> int:
    """Remove restore contents while preserving the directory and sample sources."""
    validate_layout(root)
    restore = root / 'restore'
    if not shutil.rmtree.avoids_symlink_attacks:
        raise tool.SafetyError('当前 Python 不支持安全删除目录，拒绝清理')
    # Refuse mounted filesystems, including bind mounts that can share st_dev
    try:
        mountinfo = Path('/proc/self/mountinfo').read_text(encoding='utf-8')
    except OSError as exc:
        raise tool.SafetyError(f'无法检查挂载点：{exc}') from exc
    for line in mountinfo.splitlines():
        fields = line.split()
        if len(fields) < 5:
            raise tool.SafetyError('无法解析挂载点，拒绝清理')
        mount = re.sub(r'\\([0-7]{3})', lambda match: chr(int(match.group(1), 8)), fields[4])
        if mount == str(restore) or mount.startswith(str(restore) + '/'):
            raise tool.SafetyError(f'恢复目录内存在挂载点，拒绝清理：{mount}')
    count = 0
    with os.scandir(restore) as entries:
        for entry in entries:
            if entry.is_dir(follow_symlinks=False):
                shutil.rmtree(entry.path)
            else:
                os.unlink(entry.path)
            count += 1
    return count


def validate_restore_target(root: Path, value: str) -> Path:
    restore_root = root / "restore"
    requested = Path(value).expanduser()
    if not requested.is_absolute():
        requested = Path.cwd() / requested
    candidate = Path(os.path.abspath(requested))
    try:
        relative = candidate.relative_to(restore_root)
    except ValueError as exc:
        raise tool.SafetyError("恢复目录必须位于 DBackup-Demo/restore 下") from exc
    if not relative.parts:
        raise tool.SafetyError("请指定 restore 下某个独立恢复目录")
    current = restore_root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise tool.SafetyError(f"恢复路径不能经过符号链接：{current}")
    if not candidate.is_dir():
        raise tool.SafetyError(f"恢复目录不存在或不是目录：{candidate}")
    return candidate


def comparison_report(source_name: str, restored_value: str, root: Path,
                      tolerance_seconds: float) -> int:
    validate_layout(root)
    source = root / source_name
    if source.is_symlink() or not source.is_dir():
        raise tool.SafetyError(f"来源目录不存在或不是普通目录：{source}")
    restored = validate_restore_target(root, restored_value)

    expected = tool.build_manifest(source)
    actual = tool.build_manifest(restored)
    skipped = [entry for entry in expected["entries"] if entry["kind"] == "unreadable"]
    skipped_paths = {entry["path"] for entry in skipped}
    expected["entries"] = [entry for entry in expected["entries"]
                            if entry["path"] not in skipped_paths]
    actual["entries"] = [entry for entry in actual["entries"]
                         if entry["path"] not in skipped_paths]
    results = tool.compare_manifests(expected, actual, int(tolerance_seconds * 1_000_000_000))
    for entry in skipped:
        results.append({"status": "SKIP", "category": "Unreadable source", "path": entry["path"],
                        "detail": entry.get("error", "来源文件无法读取，内容未参与比较")})

    build_dir_value = os.environ.get("DBACKUP_BUILD_DIR", str(PROJECT / "out/build-test"))
    build_dir = Path(build_dir_value).expanduser()
    if not build_dir.is_absolute():
        build_dir = PROJECT / build_dir
    report_dir = build_dir / "demo-reports"
    report = report_dir / f"{source_name}-comparison.md"
    tool.write_report(report, f"DBackup {source_name} 恢复比较", " ".join(sys.argv), results,
                      [str(source), str(restored)])
    counts = {status: sum(1 for item in results if item["status"] == status)
              for status in ("PASS", "FAIL", "SKIP")}
    print(f"比较报告：{report}")
    print(f"PASS {counts['PASS']}，FAIL {counts['FAIL']}，SKIP {counts['SKIP']}")
    return 1 if counts["FAIL"] else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("compare",))
    parser.add_argument("source", choices=("common", "large-data", "special-files"))
    parser.add_argument("restored")
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--mtime-tolerance", type=float, default=0.0)
    args = parser.parse_args()
    if args.mtime_tolerance < 0:
        raise ValueError("mtime tolerance must not be negative")
    return comparison_report(args.source, args.restored, canonical_root(args.root), args.mtime_tolerance)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, tool.SafetyError, DemoError) as exc:
        print(f"Comparison failed: {exc}", file=sys.stderr)
        raise SystemExit(1)

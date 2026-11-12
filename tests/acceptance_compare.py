#!/usr/bin/env python3
# 对比验收场景的预期与实际结果
"""Run the independent filesystem comparison with DBackup path semantics."""

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "acceptance"))
import dbackup_acceptance as tool  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", required=True)
    parser.add_argument("--restored", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--mtime-tolerance", type=float, default=2.0)
    parser.add_argument("--single-directory-layout", choices=("flatten", "preserve"), default="flatten")
    parser.add_argument("--skip-unreadable", action="store_true", help="跳过来源中无法读取的文件")
    args = parser.parse_args()
    first_source = Path(args.source[0]).resolve()
    root = next((parent for parent in (first_source, *first_source.parents)
                 if (parent / tool.MARKER).is_file()), None)
    if root is None:
        raise tool.SafetyError("来源不在带有效标记的验收测试根目录内")
    tool.load_marker(root)
    for value in [*args.source, args.restored, args.report]:
        path = Path(value).resolve()
        if path == root or root not in path.parents:
            raise tool.SafetyError(f"比较输入和报告必须位于验收根目录内: {path}")
    original = tool.build_source_manifest
    def source_manifest(values):
        result = (expected_source_manifest_with_original(values, original)
                  if args.single_directory_layout == "flatten" else original(values))
        if args.skip_unreadable:
            skipped = [entry["path"] for entry in result["entries"] if entry["kind"] == "unreadable"]
            for path in skipped:
                print(f"SKIP: 无法读取的来源文件：{path}")
            result["entries"] = [entry for entry in result["entries"] if entry["kind"] != "unreadable"]
        return result
    tool.build_source_manifest = source_manifest
    return tool.command_compare(args)


def expected_source_manifest_with_original(sources: list[str], original) -> dict:
    if len(sources) == 1 and Path(sources[0]).is_dir() and not Path(sources[0]).is_symlink():
        return tool.build_manifest(Path(sources[0]))
    return original(sources)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except tool.SafetyError as exc:
        print(f"SAFETY: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_SAFETY)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_USAGE)

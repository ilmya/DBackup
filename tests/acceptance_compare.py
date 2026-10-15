#!/usr/bin/env python3
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
    tool.build_source_manifest = lambda values: (
        expected_source_manifest_with_original(values, original)
        if args.single_directory_layout == "flatten" else original(values))
    return tool.command_compare(args)


def expected_source_manifest_with_original(sources: list[str], original) -> dict:
    expected = original(sources)
    if len(sources) != 1:
        return expected
    source = Path(sources[0])
    if tool.entry_type(source, tool.get_attributes(source)) != "directory":
        return expected
    prefix = source.name
    entries = []
    for entry in expected["entries"]:
        if entry["path"] == prefix:
            continue
        if entry["path"].startswith(prefix + "/"):
            entries.append({**entry, "path": entry["path"][len(prefix) + 1:]})
        else:
            entries.append(entry)
    return {**expected, "entries": entries}


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except tool.SafetyError as exc:
        print(f"SAFETY: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_SAFETY)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_USAGE)

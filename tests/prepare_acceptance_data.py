#!/usr/bin/env python3
"""Create the compact, repeatable Windows acceptance corpus."""

import argparse
import importlib.util
from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "tools" / "acceptance" / "dbackup_acceptance.py"
SPEC = importlib.util.spec_from_file_location("dbackup_acceptance", TOOL_PATH)
tool = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(tool)


def compact_standard(root: Path) -> None:
    normal = root / "fixtures" / "normal"
    (normal / "large-64m.bin").unlink(missing_ok=True)
    tool.write_pattern(normal / "multi-block.bin", 8 * 1024 * 1024, "random", 2718)

    compression = root / "fixtures" / "compression"
    tool.write_pattern(compression / "repeat-16m.bin", 4 * 1024 * 1024, "repeat", 11)
    tool.write_pattern(compression / "random-16m.bin", 4 * 1024 * 1024, "random", 17)
    (compression / "repeat-16m.bin").rename(compression / "repeat-4m.bin")
    (compression / "random-16m.bin").rename(compression / "random-4m.bin")

    filters = root / "fixtures" / "filters"
    for item in list(filters.iterdir()):
        if item.is_dir() and not item.is_symlink():
            shutil.rmtree(item)
        elif item.is_symlink() or item.is_file():
            item.unlink()
    matrix = (
        ("documents/report-final.txt", 1024, 0),
        ("documents/manual.pdf", 1024**2, 3),
        ("images/photo.jpg", 5 * 1024**2, 10),
        ("images/REPORT-UPPER.PNG", 10 * 1024**2, 40),
        ("audio/sample.mp3", 2048, 0),
        ("video/sample.mp4", 4096, 3),
        ("archives/sample.zip", 8192, 10),
        ("code/main.cpp", 512, 40),
        ("code/app.py", 768, 0),
        ("cache/debug.tmp", 256, 3),
        ("build/output.obj", 384, 10),
        ("names/unrelated.dat", 640, 40),
    )
    for relative, size, days in matrix:
        target = filters / relative
        seed = sum(relative.encode("utf-8"))
        tool.write_pattern(target, size, "increment", seed)
        tool.set_mtime(target, days)

    network = root / "fixtures" / "network"
    tool.write_pattern(network / "alice/multi-block.bin", 8 * 1024 * 1024, "random", 501)
    shutil.copyfile(network / "alice/multi-block.bin", network / "alice/duplicate.bin")
    tool.write_pattern(network / "bob/private.bin", 1024 * 1024, "random", 502)

    tool.save_json(root / "manifests/incremental-v1.json",
                   tool.build_manifest(root / "fixtures/incremental"))
    tool.save_json(root / "manifests/generated-fixtures.json", tool.build_manifest(root / "fixtures"))
    tool.save_json(root / "manifests/scenario-fixtures.json", {
        "schema": 1,
        "cases": {
            "S1-PACK-001": ["fixtures/normal", "fixtures/compression"],
            "S1-PACK-005": ["fixtures/normal/before-block.bin", "fixtures/normal/exact-block.bin",
                            "fixtures/normal/after-block.bin", "fixtures/normal/multi-block.bin"],
            "S2-COMP-001": ["fixtures/compression/repeat-4m.bin"],
            "S2-COMP-002": ["fixtures/compression/random-4m.bin"],
            "S2-FILTER-COMB-001": ["fixtures/filters"],
            "S5-INCR-001": ["fixtures/incremental"],
            "S4-REMOTE-001": ["fixtures/network/alice"],
            "S4-AUTH-003": ["fixtures/network/alice", "fixtures/network/bob"],
        },
    })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--profile", choices=("standard", "stress"), default="standard")
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    root = tool.normalize_root(args.root)
    tool.generate_fixtures(root, args.profile, args.reset)
    if args.profile == "standard":
        compact_standard(root)
    print(f"Compact acceptance corpus ready: {root}")
    print("Standard profile: deterministic boundary, compression, filter, incremental, and remote fixtures.")
    return tool.EXIT_OK


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except tool.SafetyError as exc:
        print(f"SAFETY: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_SAFETY)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(tool.EXIT_USAGE)

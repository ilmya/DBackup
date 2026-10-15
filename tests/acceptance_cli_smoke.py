#!/usr/bin/env python3
"""Exercise the development CLI against the independent fixture comparator."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "acceptance"))
import dbackup_acceptance as tool  # noqa: E402


def invoke(cli: Path, command: list[str], env: dict[str, str]) -> dict:
    result = subprocess.run([str(cli), *command], cwd=ROOT, env=env,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=90)
    if result.returncode != 0:
        raise RuntimeError(f"CLI failed ({result.returncode}): {result.stderr}\n{result.stdout}")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"CLI did not return JSON: {result.stdout}\n{result.stderr}") from exc
    if not value.get("ok"):
        raise RuntimeError(f"CLI returned failure JSON: {value}")
    return value


def compare_tree(source: Path, restored: Path, report: Path, env: dict[str, str], *, layout: str) -> None:
    command = [sys.executable, str(ROOT / "tests" / "acceptance_compare.py"),
               "--source", str(source), "--restored", str(restored), "--report", str(report),
               "--single-directory-layout", layout]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30)
    if result.returncode:
        raise RuntimeError(f"Independent compare failed: {result.stdout}\n{result.stderr}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--test-cli", required=True)
    args = parser.parse_args()

    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    cli = Path(args.test_cli).resolve()
    if not cli.is_file():
        raise FileNotFoundError(cli)
    env = os.environ.copy()
    env["DBACKUP_TEST_PASSWORD"] = "DBackup-Acceptance-Only!2026"
    env["PYTHONUTF8"] = "1"

    run_id = uuid.uuid4().hex
    normal = root / "fixtures" / "normal"
    archive = root / "archives" / f"cli-smoke-{run_id}.abk"
    restored_archive = root / "restore" / f"cli-archive-{run_id}"
    invoke(cli, ["archive", "preview", "--root", str(root), "--source", str(normal)], env)
    invoke(cli, ["archive", "create", "--root", str(root), "--source", str(normal),
                 "--output", str(archive), "--compression", "6"], env)
    inspected = invoke(cli, ["archive", "inspect", "--root", str(root), "--input", str(archive)], env)
    if inspected["data"].get("count", 0) <= 0:
        raise RuntimeError("Archive inspect returned no entries")
    invoke(cli, ["archive", "extract", "--root", str(root), "--input", str(archive),
                 "--output", str(restored_archive)], env)
    compare_tree(normal, restored_archive, root / "reports" / f"cli-archive-{run_id}.md", env,
                 layout="flatten")

    source = root / "fixtures" / "incremental"
    repository = root / "repositories" / f"cli-smoke-{run_id}"
    snapshot = invoke(cli, ["repository", "snapshot", "--root", str(root), "--repository", str(repository),
                            "--source", str(source)], env)["data"]["id"]
    listed = invoke(cli, ["repository", "list", "--root", str(root), "--repository", str(repository)], env)
    if not any(item.get("id") == snapshot for item in listed["data"].get("snapshots", [])):
        raise RuntimeError("Repository list omitted the newly committed snapshot")
    restored_repository = root / "restore" / f"cli-repository-{run_id}"
    invoke(cli, ["repository", "restore", "--root", str(root), "--repository", str(repository),
                 "--id", snapshot, "--output", str(restored_repository)], env)
    compare_tree(source, restored_repository, root / "reports" / f"cli-repository-{run_id}.md", env,
                 layout="preserve")

    print(json.dumps({"ok": True, "command": "acceptance-cli-smoke",
                      "archiveEntries": inspected["data"]["count"],
                      "repositorySnapshot": snapshot,
                      "archiveRestoreMatches": True, "repositoryRestoreMatches": True},
                     ensure_ascii=False, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

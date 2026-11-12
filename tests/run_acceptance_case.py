#!/usr/bin/env python3
# 执行指定验收用例并检查结果
"""Run the CTest group mapped to one automated acceptance case."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "tests" / "acceptance-scenarios.json"


def ctest_pattern(case_id: str) -> str:
    if case_id in {"S1-PACK-001", "S1-RESTORE-001", "S2-COMP-001", "S2-COMP-002", "S2-CRYPT-001"}:
        return "^(AcceptanceTestCliSmoke|ArchiveServiceTest\\.(CompressedEncryptedRoundTrip|LargeFileRoundTrip)|ArchiveV3Test\\.UsesAuthenticatedEncryptionAndSupportsCancellation)$"
    if case_id.startswith("S1-PACK-") or case_id.startswith("S1-RESTORE-"):
        return "^ArchiveServiceTest\\."
    if case_id.startswith("S2-FILTER-"):
        return "^ArchiveServiceTest\\.(FilterAcrossAllSixCategories|PreviewWithMultiExtensionAndWildcardRules)$"
    if case_id.startswith("S3-CRON-"):
        return "^JobManagerTest\\."
    if case_id.startswith("S3-REALTIME-"):
        return "^DirectoryWatcherTest\\."
    if case_id.startswith("S3-META-"):
        return "^ArchiveV3Test\\.PreservesSafeRelativeSymbolicLink$"
    incremental_cases = {
        "S5-INCR-001": "^IncrementalChain\\.CreatesListsAndRestoresVersionsWithoutOverwriting$",
        "S5-INCR-002": "^IncrementalDiff\\.FindsAddedChangedAndDeletedPaths$",
        "S5-INCR-003": "^IncrementalScan\\.DetectsSameSizeAndTimestampContentChange$",
        "S5-INCR-004": "^IncrementalChain\\.RestoresFileDirectoryAndSymlinkTransitions$",
        "S5-INCR-007": "^IncrementalChain\\.StrictRetentionRebasesOldestKeptVersion$",
    }
    if case_id in incremental_cases:
        return incremental_cases[case_id]
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--case")
    selector.add_argument("--all", action="store_true")
    args = parser.parse_args()

    if args.all:
        result = subprocess.run(["bash", "build.sh", "test"], cwd=ROOT)
        return result.returncode

    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    automated = {item["id"] for item in catalog.get("automatedCases", [])}
    pattern = ctest_pattern(args.case)
    if args.case not in automated or not pattern:
        print(f"{args.case} is not registered as an automated case; execute its documented manual procedure.",
              file=sys.stderr)
        return 2

    ctest = shutil.which("ctest")
    if not ctest:
        print("CTest was not found. Run `./build.sh test` first or add CMake to PATH.", file=sys.stderr)
        return 2
    build_dir = ROOT / "out" / "build-test"
    if not (build_dir / "CTestTestfile.cmake").is_file():
        print("The test build is missing. Run `./build.sh test` first.", file=sys.stderr)
        return 2

    if "AcceptanceTestCliSmoke" in pattern:
        generated = subprocess.run([ctest, "--test-dir", str(build_dir), "-R", "^AcceptanceFixtureGenerate$",
                                    "--output-on-failure"], cwd=ROOT)
        if generated.returncode:
            return generated.returncode
    print(f"Acceptance case {args.case}; CTest selection: {pattern}")
    return subprocess.run([ctest, "--test-dir", str(build_dir), "-R", pattern,
                           "--output-on-failure"], cwd=ROOT).returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)

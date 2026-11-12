#!/usr/bin/env python3
# 汇总并报告 gcov 测试覆盖率
"""Create a GCC gcov line/branch summary for production code under test."""

from __future__ import annotations

import argparse
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import sys


PROJECT = Path(__file__).resolve().parents[1]
CORE_SOURCES = (
    "core/archive/archive.cpp",
    "core/archive/archive_pipeline.cpp",
    "core/archive/archive_tar.cpp",
    "core/selection/backup_policy.cpp",
    "core/compression/compression.cpp",
    "core/security/md5.cpp",
    "core/security/crypto.cpp",
    "core/filesystem/linux_file.cpp",
    "core/security/credential_store.cpp",
    "automation/schedule/cron.cpp",
    "automation/realtime/directory_watcher.cpp",
)
AUTOMATION_TEST_SOURCES = (
    "automation/job_manager.cpp",
)


def find_object(build_dir: Path, source_relative: str, target_fragment: str) -> Path:
    expected = Path(source_relative).name + ".gcno"
    candidates = [
        path for path in build_dir.rglob(expected)
        if target_fragment in path.as_posix()
    ]
    if not candidates:
        raise RuntimeError(f"未找到 {source_relative} 的 GCC 覆盖率对象（{target_fragment}）")
    return sorted(candidates)[0]


def read_counts(report: Path) -> dict[str, dict[str, int | str]]:
    with gzip.open(report, "rt", encoding="utf-8") as stream:
        data = json.load(stream)
    counts: dict[str, dict[str, int | str]] = {}
    for item in data.get("files", []):
        source_path = Path(item.get("file", "unknown"))
        source_path = source_path.resolve() if source_path.is_absolute() else (PROJECT / source_path).resolve()
        try:
            relative_source = source_path.relative_to(PROJECT)
        except ValueError:
            continue
        if not relative_source.parts or relative_source.parts[0] not in {"core", "automation"}:
            continue
        filename = relative_source.as_posix()
        lines: dict[int, int] = {}
        branches: dict[tuple[int, int], int] = {}
        for line in item.get("lines", []):
            line_number = int(line.get("line_number", 0))
            if line_number > 0:
                lines[line_number] = max(lines.get(line_number, 0), int(line.get("count", 0)))
            for index, branch in enumerate(line.get("branches", [])):
                key = (line_number, index)
                branches[key] = max(branches.get(key, 0), int(branch.get("count", 0)))
        counts[filename] = {
            "line_total": len(lines),
            "line_covered": sum(count > 0 for count in lines.values()),
            "branch_total": len(branches),
            "branch_covered": sum(count > 0 for count in branches.values()),
        }
    return counts


def collect(gcov: str, build_dir: Path, output_dir: Path,
            source_relative: str, target_fragment: str) -> dict[str, dict[str, int | str]]:
    source = PROJECT / source_relative
    object_file = find_object(build_dir, source_relative, target_fragment)
    source_output = output_dir / Path(source_relative).parent
    source_output.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [gcov, "--json-format", "--branch-probabilities", "--branch-counts",
         "--object-directory", str(object_file), str(source)],
        cwd=source_output,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode:
        raise RuntimeError(f"gcov 处理 {source_relative} 失败：\n{completed.stdout}")
    reports = sorted(source_output.glob("*.gcov.json.gz"), key=lambda path: path.stat().st_mtime_ns)
    if not reports:
        raise RuntimeError(f"gcov 未为 {source_relative} 生成 JSON 报告：\n{completed.stdout}")
    report = reports[-1]
    counts = read_counts(report)
    if not counts:
        raise RuntimeError(f"gcov 报告中没有源代码行：{report}")
    return counts


def format_percent(covered: int, total: int) -> str:
    if not total:
        return "n/a"
    return f"{covered * 100.0 / total:.1f}%"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--gcov", default=shutil.which("gcov") or "gcov")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    gcov_output = output_dir / "gcov"
    gcov_output.mkdir(exist_ok=True)

    all_counts: dict[str, dict[str, int | str]] = {}
    try:
        for source in CORE_SOURCES:
            all_counts.update(collect(args.gcov, build_dir, gcov_output, source, "dbackup_core.dir"))
        for source in AUTOMATION_TEST_SOURCES:
            all_counts.update(collect(args.gcov, build_dir, gcov_output, source,
                                      "BackupToolAutomationTests.dir"))
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"覆盖率报告生成失败：{exc}", file=sys.stderr)
        return 1

    totals = {key: sum(int(item[key]) for item in all_counts.values())
              for key in ("line_covered", "line_total", "branch_covered", "branch_total")}
    report_path = output_dir / "summary.md"
    with report_path.open("w", encoding="utf-8") as report:
        report.write("# GCC 覆盖率摘要\n\n")
        report.write("此报告仅统计本次 `unit` 标签运行覆盖到的生产代码行和分支，不设置门槛，也不能证明功能正确。\n\n")
        report.write("| 源文件 | 行覆盖 | 分支覆盖 |\n|---|---:|---:|\n")
        for filename, item in sorted(all_counts.items()):
            report.write(
                f"| `{filename}` | {item['line_covered']}/{item['line_total']} "
                f"({format_percent(int(item['line_covered']), int(item['line_total']))}) | "
                f"{item['branch_covered']}/{item['branch_total']} "
                f"({format_percent(int(item['branch_covered']), int(item['branch_total']))}) |\n"
            )
        report.write(
            f"\n| 合计 | {totals['line_covered']}/{totals['line_total']} "
            f"({format_percent(totals['line_covered'], totals['line_total'])}) | "
            f"{totals['branch_covered']}/{totals['branch_total']} "
            f"({format_percent(totals['branch_covered'], totals['branch_total'])}) |\n"
        )
        report.write("\nGUI 界面、测试程序自身和第三方代码未纳入统计。\n")
    print(f"覆盖率摘要：{report_path}")
    print(f"行覆盖：{totals['line_covered']}/{totals['line_total']} ({format_percent(totals['line_covered'], totals['line_total'])})")
    print(f"分支覆盖：{totals['branch_covered']}/{totals['branch_total']} ({format_percent(totals['branch_covered'], totals['branch_total'])})")
    print(f"gcov JSON 明细：{gcov_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

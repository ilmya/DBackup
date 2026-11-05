#!/usr/bin/env python3
"""Create and compare a persistent, isolated Linux backup demonstration."""
import argparse
import errno
import json
import os
from pathlib import Path
import random
import socket
import stat
import subprocess
import sys
import uuid

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools" / "acceptance"))
import dbackup_acceptance as tool


def add_extra_cases(source):
    extra = source / "extra"
    if extra.exists() or extra.is_symlink():
        return
    extra.mkdir()
    (extra / "space name.txt").write_text("A filename with spaces\n", encoding="utf-8")
    (extra / "compressible.txt").write_bytes(b"DBackup sample\n" * 4096)
    (extra / "random.bin").write_bytes(random.Random(24).randbytes(32768))
    with (extra / "sparse.bin").open("wb") as stream:
        stream.seek(1024 * 1024)
        stream.write(b"end")
    script = extra / "run.sh"
    script.write_text("#!/bin/sh\nprintf 'sample\\n'\n", encoding="utf-8")
    script.chmod(0o750)
    nested = extra / "nested" / "deep"
    nested.mkdir(parents=True)
    (nested / "file.txt").write_text("nested file\n", encoding="utf-8")
    stamp = 1700000000123456789
    for path in sorted(extra.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        os.utime(path, ns=(stamp, stamp), follow_symlinks=False)
    os.utime(extra, ns=(stamp, stamp))


def add_permission_case(source):
    directory = source / "dbackup_permission_case"
    if directory.exists() or directory.is_symlink():
        return
    directory.mkdir()
    (directory / "readable.txt").write_text("readable\n", encoding="utf-8")
    restricted = directory / "no_read.txt"
    restricted.write_text("unreadable for ordinary users\n", encoding="utf-8")
    restricted.chmod(0)


def prepare(root):
    if root.is_symlink():
        raise tool.SafetyError("演示根目录不能是符号链接")
    if root.exists():
        tool.load_marker(root)
        cases = root / "cases"
        if cases.is_symlink():
            raise tool.SafetyError("案例目录不能是符号链接")
        cases.mkdir(exist_ok=True)
        source = root / "test"
        if source.is_symlink() or not source.is_dir():
            raise tool.SafetyError("案例目录不存在或是符号链接")
        add_extra_cases(source)
        add_permission_case(source)
        print(f"沿用已有演示目录，不覆盖已有案例：{root}")
        return
    root.mkdir(parents=True)
    tool.write_marker(root)
    for name in ("test", "restore", "archives", "reports", "tmp", "cases"):
        (root / name).mkdir()
    source = root / "test"
    (source / "docs" / "empty_dir").mkdir(parents=True)
    (source / "docs" / "note.txt").write_text("DBackup sample data\nSecond line\n", encoding="utf-8")
    (source / "binary.bin").write_bytes(bytes(range(256)) * 4)
    (source / "empty.txt").touch()
    os.chmod(source / "docs", 0o750)
    os.chmod(source / "docs" / "note.txt", 0o640)
    os.link(source / "docs" / "note.txt", source / "hardlink.txt")
    os.symlink("docs/note.txt", source / "symlink")
    os.mkfifo(source / "pipe.fifo", 0o640)
    timestamp = 1700000000123456789
    for path in sorted(source.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        os.utime(path, ns=(timestamp, timestamp), follow_symlinks=False)
    add_extra_cases(source)
    add_permission_case(source)
    print(f"已创建展示案例：{source}")
    print("包含普通文件、空文件和目录、权限、时间、符号链接、硬链接和 FIFO。")
    print("不可读文件用于观察部分完成和跳过提示；设备与套接字另有独立案例。")


def run_command(args):
    print("$ " + " ".join(map(str, args)), flush=True)
    subprocess.run([str(value) for value in args], check=True)


def run_scenarios(root, cli, run_id):
    """Keep incompatible cases separate from the normal round trip."""
    cases = root / "cases"
    if cases.is_symlink():
        raise tool.SafetyError("案例目录不能是符号链接")
    cases.mkdir(exist_ok=True)
    case = cases / run_id
    case.mkdir(mode=0o755)
    report = root / "reports" / (run_id + "-cases.md")
    results = []

    def add(status, category, path, detail):
        results.append(dict(status=status, category=category, path=str(path), detail=str(detail)))
        print(f"{status}: {category}: {detail}", flush=True)

    def call(*args):
        completed = subprocess.run([str(cli), *map(str, args)], text=True,
                                   capture_output=True, env={**os.environ, "DBACKUP_TEST_PASSWORD": ""})
        try:
            data = json.loads(completed.stdout)
        except json.JSONDecodeError:
            return False, f"测试程序输出无效（退出码 {completed.returncode}）：{completed.stderr}"
        ok = completed.returncode == 0 and data.get("ok") is True
        return ok, data.get("error") or data.get("data", {}).get("warning") or "完成"

    def create(source, output):
        return call("archive", "create", "--root", root, "--source", source, "--output", output)

    socket_dir = case / "socket"
    socket_dir.mkdir()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.bind(str(socket_dir / "endpoint.sock"))
        socket_archive = case / "socket.dbak"
        ok, detail = create(socket_dir, socket_archive)
        if ok:
            socket_restore = root / "restore" / (run_id + "-socket")
            ok, detail = call("archive", "extract", "--root", root, "--input", socket_archive, "--output", socket_restore)
            ok = ok and stat.S_ISSOCK((socket_restore / "endpoint.sock").lstat().st_mode)
    add("PASS" if ok else "FAIL", "Socket roundtrip", socket_dir, detail)

    restricted = case / "restricted"
    restricted.mkdir()
    restricted_file = restricted / "no_read.txt"
    restricted_file.write_text("Permission test\n", encoding="utf-8")
    if os.geteuid() == 0:
        try:
            os.chown(restricted_file, 65534, 65534)
        except PermissionError as exc:
            add("SKIP", "Different owner setup", restricted_file, str(exc))
    restricted_file.chmod(0)
    restricted_archive = case / "restricted.dbak"
    ok, detail = create(restricted, restricted_archive)
    if os.geteuid() == 0:
        if ok:
            destination = root / "restore" / (run_id + "-restricted")
            ok, detail = call("archive", "extract", "--root", root, "--input", restricted_archive,
                              "--output", destination)
            if ok:
                actual = (destination / "no_read.txt").lstat()
                expected = restricted_file.lstat()
                ok = (stat.S_IMODE(actual.st_mode) == 0 and
                      (actual.st_uid, actual.st_gid) == (expected.st_uid, expected.st_gid))
                detail = "恢复成功，权限及属主一致" if ok else "恢复后的权限或属主不一致"
        add("PASS" if ok else "FAIL", "Privileged restricted file", restricted_file, detail)
    else:
        add("PASS" if not ok and ("权限" in detail or "Permission denied" in detail) else "FAIL",
            "Unreadable file rejection", restricted_file, detail)
        add("SKIP", "Privileged restricted file", restricted_file, "需要 sudo 执行提权案例")

    devices = case / "devices"
    devices.mkdir()
    available = []
    for name, kind, number in (("char.dev", stat.S_IFCHR, os.makedev(1, 3)),
                               ("block.dev", stat.S_IFBLK, os.makedev(7, 0))):
        node = devices / name
        try:
            os.mknod(node, kind, number)
            available.append(node)
        except OSError as exc:
            status = "SKIP" if exc.errno in (errno.EPERM, errno.EACCES, errno.ENOTSUP, errno.EOPNOTSUPP) else "FAIL"
            add(status, "Create device", node, f"{exc.strerror}；需要相应能力及文件系统支持")
    if available:
        device_archive = case / "devices.dbak"
        ok, detail = create(devices, device_archive)
        if not ok:
            add("FAIL", "Archive devices", devices, detail)
        else:
            dest = root / "restore" / (run_id + "-devices")
            skipped = root / "restore" / (run_id + "-devices-skipped")
            ok, detail = call("archive", "extract", "--root", root, "--input", device_archive, "--output", skipped, "--skip-device-nodes")
            add("PASS" if ok and "设备节点" in detail else "FAIL", "Device optional skip", devices, detail)
            ok, detail = call("archive", "extract", "--root", root, "--input", device_archive,
                              "--output", dest, "--allow-device-nodes")
            if ok:
                differences = tool.compare_manifests(tool.build_manifest(devices), tool.build_manifest(dest), 0)
                failures = [item for item in differences if item["status"] == "FAIL"]
                ok = not failures
                detail = f"{len(differences)} 项比较，{len(failures)} 项失败" if ok else str(failures)
            elif os.geteuid() != 0 and ("权限" in detail or "Operation not permitted" in detail):
                add("SKIP", "Device restore", devices, detail)
                ok = None
            if ok is not None:
                add("PASS" if ok else "FAIL", "Device restore", devices, detail)
    else:
        add("SKIP", "Device optional skip", devices, "没有可用设备节点案例")
        add("SKIP", "Device restore", devices, "没有可用设备节点案例")
    tool.write_report(report, "DBackup 特殊文件及权限案例", "tests/demo.sh scenarios", results, [str(case)])
    print(f"特殊案例报告：{report}")
    return not any(item["status"] == "FAIL" for item in results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "run", "compare", "scenarios"))
    parser.add_argument("restored", nargs="?", help="compare 时指定恢复目录")
    parser.add_argument("--root", default=os.environ.get("DBACKUP_DEMO_ROOT", str(Path.home() / "DBackup-Demo")))
    parser.add_argument("--cli", default=os.environ.get("DBACKUP_DEMO_CLI", str(PROJECT / "out/build-test/BackupToolTestCli")))
    args = parser.parse_args()
    root = Path(args.root).expanduser().absolute()
    tool.normalize_root(str(root))
    if args.command == "prepare":
        prepare(root)
        return
    tool.load_marker(root)
    source = root / "test"
    if not source.is_dir() or source.is_symlink():
        raise tool.SafetyError("案例目录不存在或是符号链接")
    for name in ("restore", "archives", "reports", "tmp"):
        if (root / name).is_symlink() or not (root / name).is_dir():
            raise tool.SafetyError(f"演示子目录不存在或是符号链接：{root / name}")
    run_id = "run-" + uuid.uuid4().hex[:8]
    if args.command == "scenarios":
        if args.restored:
            parser.error("scenarios 不接受恢复目录")
        if not run_scenarios(root, Path(args.cli), run_id):
            sys.exit(1)
        return
    if args.command == "run":
        if args.restored:
            parser.error("run 不接受恢复目录")
        restored = root / "restore" / run_id
        archive = root / "archives" / (run_id + ".dbak")
        env_password = os.environ.get("DBACKUP_TEST_PASSWORD")
        if env_password:
            raise tool.SafetyError("请先取消 DBACKUP_TEST_PASSWORD，展示案例不使用环境中的密码")
        cli = Path(args.cli)
        run_command([cli, "archive", "create", "--root", root, "--source", source, "--output", archive])
        run_command([cli, "archive", "extract", "--root", root, "--input", archive, "--output", restored])
    else:
        if not args.restored:
            parser.error("compare 需要指定恢复目录")
        restored = Path(args.restored).expanduser().resolve()
        restore_root = (root / "restore").resolve()
        if restored == restore_root or restore_root not in restored.parents:
            raise tool.SafetyError("恢复目录必须位于演示目录的 restore 子目录中")
    report = root / "reports" / (run_id + ".md")
    run_command([sys.executable, PROJECT / "tests/acceptance_compare.py", "--source", source,
                 "--restored", restored, "--report", report, "--mtime-tolerance", "0", "--skip-unreadable"])
    print(f"来源：{source}\n恢复：{restored}\n报告：{report}")
    if args.command == "run" and not run_scenarios(root, Path(args.cli), run_id):
        sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tool.SafetyError, subprocess.CalledProcessError) as exc:
        print(f"演示未完成：{exc}", file=sys.stderr)
        sys.exit(1)

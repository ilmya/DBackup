#!/usr/bin/env python3
"""Independent acceptance-data and comparison tool for DBackup.

This program deliberately does not import or inspect DBackup code or manifests.
It uses only Python's standard library and documented Windows APIs/commands.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import datetime as dt
import getpass
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import ssl
import urllib.error
import urllib.request


TOOL_VERSION = "1.0"
MARKER = ".dbackup-acceptance.json"
DEFAULT_ROOT = Path(r"D:\DBackup-Acceptance")
EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_BLOCKED, EXIT_SAFETY = 0, 1, 2, 3, 4
FILE_ATTRIBUTE_READONLY = 0x1
FILE_ATTRIBUTE_HIDDEN = 0x2
FILE_ATTRIBUTE_ARCHIVE = 0x20
FILE_ATTRIBUTE_REPARSE_POINT = 0x400


class SafetyError(RuntimeError):
    pass


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def win_long(path: Path) -> str:
    # Path.resolve() follows reparse points and would inspect the target instead
    # of the link itself. Get an absolute spelling without dereferencing it.
    value = os.path.abspath(os.fspath(path))
    return value if value.startswith("\\\\?\\") else "\\\\?\\" + value


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True,
                          encoding="utf-8", errors="replace", check=False)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def normalize_root(value: str | None, *, require_drive: bool = True) -> Path:
    root = Path(value).expanduser().resolve() if value else DEFAULT_ROOT.resolve()
    if value is None and require_drive and not Path(root.drive + "\\").exists():
        raise SafetyError("默认 D: 盘不存在，请显式传入 --root <安全目录>")
    anchor = Path(root.anchor).resolve()
    home = Path.home().resolve()
    repository = repo_root()
    if root == anchor or root == home or root == repository:
        raise SafetyError(f"拒绝使用危险测试根目录: {root}")
    if root in repository.parents:
        raise SafetyError(f"测试根目录不能包含仓库: {root}")
    return root


def marker_path(root: Path) -> Path:
    return root / MARKER


def load_marker(root: Path) -> dict:
    path = marker_path(root)
    if not path.is_file():
        raise SafetyError(f"目录缺少 {MARKER}，拒绝执行危险操作: {root}")
    try:
        marker = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SafetyError(f"安全标记无法读取: {exc}") from exc
    if marker.get("kind") != "dbackup-acceptance-root" or marker.get("version") != 1:
        raise SafetyError("安全标记类型或版本不正确")
    try:
        uuid.UUID(marker["uuid"])
    except (KeyError, ValueError, TypeError) as exc:
        raise SafetyError("安全标记 UUID 无效") from exc
    if Path(marker.get("root", "")).resolve() != root.resolve():
        raise SafetyError("安全标记记录的根目录与实际目录不一致")
    return marker


def write_marker(root: Path) -> dict:
    marker = {"kind": "dbackup-acceptance-root", "version": 1,
              "uuid": str(uuid.uuid4()), "root": str(root.resolve()),
              "createdAt": now_iso(), "toolVersion": TOOL_VERSION}
    marker_path(root).write_text(json.dumps(marker, ensure_ascii=False, indent=2), encoding="utf-8")
    return marker


def ensure_layout(root: Path) -> None:
    for relative in ("fixtures/normal", "fixtures/compression", "fixtures/filters",
                     "fixtures/permissions", "fixtures/links", "fixtures/incremental",
                     "fixtures/realtime", "fixtures/network", "restore", "archives",
                     "repositories", "server-data", "manifests", "reports", "evidence"):
        (root / relative).mkdir(parents=True, exist_ok=True)


def write_pattern(path: Path, size: int, kind: str = "zero", seed: int = 20260925) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    block_size = 1024 * 1024
    rng = random.Random(seed)
    remaining = size
    with path.open("wb") as stream:
        while remaining:
            count = min(block_size, remaining)
            if kind == "repeat":
                block = (b"DBackup acceptance data\n" * (count // 24 + 1))[:count]
            elif kind == "increment":
                block = bytes(range(256)) * (count // 256) + bytes(range(count % 256))
            elif kind == "random":
                block = rng.randbytes(count)
            else:
                block = bytes(count)
            stream.write(block)
            remaining -= count


def set_mtime(path: Path, days_ago: int, hour: int = 12) -> None:
    moment = dt.datetime.now().astimezone().replace(hour=hour, minute=0, second=0,
                                                    microsecond=0) - dt.timedelta(days=days_ago)
    stamp = moment.timestamp()
    try:
        os.utime(path, (stamp, stamp), follow_symlinks=False)
    except NotImplementedError:
        os.utime(path, (stamp, stamp))


def set_attributes(path: Path, attributes: int) -> None:
    if os.name == "nt":
        ctypes.windll.kernel32.SetFileAttributesW(win_long(path), attributes)


def generate_fixtures(root: Path, profile: str, reset: bool) -> None:
    if root.exists() and any(root.iterdir()):
        if not marker_path(root).is_file():
            raise SafetyError(f"拒绝覆盖非空且未标记的目录: {root}")
        load_marker(root)
        if not reset:
            raise SafetyError("目录已有验收数据；如需重建请显式使用 --reset")
        safe_cleanup(root, confirmed=True)
    root.mkdir(parents=True, exist_ok=True)
    marker = write_marker(root)
    ensure_layout(root)

    normal = root / "fixtures/normal"
    (normal / "empty-dir").mkdir()
    (normal / "nested/level1/level2").mkdir(parents=True)
    (normal / "empty.txt").touch()
    (normal / "hello.txt").write_text("Hello DBackup\n", encoding="utf-8")
    (normal / "中文文件名.txt").write_text("中文内容：数据备份验收\n", encoding="utf-8")
    (normal / "name with spaces.txt").write_text("spaces\n", encoding="utf-8")
    (normal / "emoji-📦.txt").write_text("emoji filename\n", encoding="utf-8")
    (normal / "README").write_text("no extension\n", encoding="utf-8")
    (normal / "many.dots.in.name.data").write_bytes(b"dots")
    (normal / "nested/level1/level2/deep.txt").write_text("deep\n", encoding="utf-8")
    (normal / "same-a.bin").write_bytes(b"identical content")
    (normal / "same-b.bin").write_bytes(b"identical content")
    for name, size, kind in (
        ("one-byte.bin", 1, "increment"), ("one-kib.bin", 1024, "increment"),
        ("before-block.bin", 4 * 1024 * 1024 - 1, "increment"),
        ("exact-block.bin", 4 * 1024 * 1024, "increment"),
        ("after-block.bin", 4 * 1024 * 1024 + 1, "increment"),
        ("multi-block.bin", 12 * 1024 * 1024, "random"),
        ("large-64m.bin", 64 * 1024 * 1024, "random")):
        write_pattern(normal / name, size, kind)
    set_attributes(normal / "hello.txt", FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY)
    set_attributes(normal / "中文文件名.txt", FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN)

    compression = root / "fixtures/compression"
    write_pattern(compression / "repeat-16m.bin", 16 * 1024 * 1024, "repeat")
    write_pattern(compression / "random-16m.bin", 16 * 1024 * 1024, "random", 17)
    (compression / "known-plaintext.txt").write_text(
        "DBACKUP-KNOWN-PLAINTEXT-DO-NOT-USE-AS-A-REAL-SECRET\n", encoding="utf-8")

    filters = root / "fixtures/filters"
    samples = {
        "documents/report-final.txt": b"report", "documents/manual.pdf": b"fake-pdf",
        "documents/notes.md": b"notes", "images/photo.jpg": b"fake-jpeg",
        "images/icon.png": b"fake-png", "audio/sample.mp3": b"fake-mp3",
        "video/sample.mp4": b"fake-mp4", "archives/sample.zip": b"fake-zip",
        "code/main.cpp": b"int main(){}", "code/app.py": b"print('ok')",
        "logs/application.log": b"log", "cache/debug.tmp": b"tmp",
        "build/output.obj": b"obj", "names/report-2026.txt": b"match",
        "names/REPORT-UPPER.TXT": b"case", "names/unrelated.dat": b"other"}
    for relative, data in samples.items():
        target = filters / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    for name, size in (("1KiB.bin", 1024), ("1MiB.bin", 1024**2),
                       ("5MiB.bin", 5 * 1024**2), ("10MiB.bin", 10 * 1024**2)):
        write_pattern(filters / "sizes" / name, size, "increment")
    for name, days in (("today.txt", 0), ("three-days.txt", 3),
                       ("ten-days.txt", 10), ("forty-days.txt", 40)):
        target = filters / "times" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(name, encoding="utf-8")
        set_mtime(target, days)

    permissions = root / "fixtures/permissions"
    (permissions / "denied-read.txt").write_text("permission failure fixture", encoding="utf-8")
    (permissions / "readonly.txt").write_text("read only", encoding="utf-8")
    (permissions / "no-inheritance.txt").write_text("custom ACL", encoding="utf-8")
    set_attributes(permissions / "readonly.txt", FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY)

    incremental = root / "fixtures/incremental"
    write_pattern(incremental / "large-12m.bin", 12 * 1024**2, "random", 99)
    (incremental / "unchanged.txt").write_text("never changes\n", encoding="utf-8")
    (incremental / "delete-in-v2.txt").write_text("delete me\n", encoding="utf-8")
    (incremental / "rename-in-v2.txt").write_text("rename me\n", encoding="utf-8")
    (incremental / "mtime-only.txt").write_text("same content, changed time\n", encoding="utf-8")
    (incremental / "attributes-only.txt").write_text("same content, changed attributes\n", encoding="utf-8")

    realtime = root / "fixtures/realtime"
    (realtime / "README.txt").write_text("Perform realtime cases in this directory.\n", encoding="utf-8")
    write_pattern(realtime / "bulk/template.bin", 1024 * 1024, "increment")

    network = root / "fixtures/network"
    write_pattern(network / "alice/multi-block.bin", 12 * 1024**2, "random", 501)
    shutil.copyfile(network / "alice/multi-block.bin", network / "alice/duplicate.bin")
    (network / "alice/empty.bin").touch()
    write_pattern(network / "bob/private.bin", 5 * 1024**2, "random", 502)

    if profile == "stress":
        write_pattern(normal / "stress-512m.bin", 512 * 1024**2, "random", 700)
        write_pattern(normal / "stress-1g.bin", 1024**3, "repeat", 701)

    manifest = build_manifest(incremental)
    save_json(root / "manifests/incremental-v1.json", manifest)
    save_json(root / "manifests/generated-fixtures.json", build_manifest(root / "fixtures"))
    print(f"Generated DBackup acceptance data at: {root}")
    print(f"Safety marker UUID: {marker['uuid']}")
    print("Permissions and links are not activated automatically.")


def get_attributes(path: Path) -> int:
    if os.name != "nt":
        return 0
    value = ctypes.windll.kernel32.GetFileAttributesW(win_long(path))
    return int(value) if value != 0xFFFFFFFF else 0


def get_sddl(path: Path) -> str | None:
    if os.name != "nt":
        return None
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    descriptor = ctypes.c_void_p()
    advapi.GetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD,
                                             ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    advapi.GetNamedSecurityInfoW.restype = wintypes.DWORD
    result = advapi.GetNamedSecurityInfoW(str(path), 1, 0x1 | 0x2 | 0x4, None, None, None,
                                          None, ctypes.byref(descriptor))
    if result:
        return None
    text = wintypes.LPWSTR()
    length = wintypes.DWORD()
    advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
        ctypes.POINTER(wintypes.LPWSTR), ctypes.POINTER(wintypes.DWORD)]
    ok = advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor, 1, 0x1 | 0x2 | 0x4, ctypes.byref(text), ctypes.byref(length))
    value = text.value if ok else None
    if text:
        kernel.LocalFree(text)
    kernel.LocalFree(descriptor)
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def entry_type(path: Path, attributes: int) -> str:
    if attributes & FILE_ATTRIBUTE_REPARSE_POINT:
        try:
            return "junction" if path.is_dir() and not path.is_symlink() else "symlink"
        except OSError:
            return "reparse"
    return "directory" if path.is_dir() else "file"


def build_manifest(base: Path) -> dict:
    base = base.resolve()
    if not base.exists():
        raise FileNotFoundError(base)
    entries: list[dict] = []
    stack = [base]
    while stack:
        current = stack.pop()
        children = sorted(os.scandir(current), key=lambda item: item.name.casefold(), reverse=True)
        for child in children:
            path = Path(child.path)
            relative = path.relative_to(base).as_posix()
            attrs = get_attributes(path)
            kind = entry_type(path, attrs)
            try:
                info = path.lstat()
            except OSError as exc:
                entries.append({"path": relative, "kind": "unreadable", "error": str(exc)})
                continue
            entry = {"path": relative, "kind": kind, "mtimeNs": info.st_mtime_ns,
                     "attributes": attrs, "sddl": get_sddl(path)}
            if kind == "file":
                try:
                    entry.update(size=info.st_size, sha256=sha256_file(path))
                except OSError as exc:
                    entry.update(kind="unreadable", error=str(exc), size=info.st_size)
            elif kind in ("symlink", "junction", "reparse"):
                try:
                    entry["target"] = os.readlink(path)
                except OSError as exc:
                    entry["targetError"] = str(exc)
            elif kind == "directory":
                entry["empty"] = not any(os.scandir(path))
                stack.append(path)
            entries.append(entry)
    entries.sort(key=lambda item: item["path"].casefold())
    return {"schema": 1, "toolVersion": TOOL_VERSION, "base": str(base),
            "generatedAt": now_iso(), "host": socket.gethostname(), "user": getpass.getuser(),
            "platform": platform.platform(), "entries": entries}


def build_source_manifest(sources: list[str]) -> dict:
    """Build the expected restored tree for one or more selected GUI sources."""
    combined: dict[str, dict] = {}
    resolved: list[Path] = []
    for value in sources:
        source = Path(value).resolve()
        if not source.exists():
            raise FileNotFoundError(source)
        resolved.append(source)
        parent_manifest = build_manifest(source.parent)
        prefix = source.name
        selected = [entry for entry in parent_manifest["entries"]
                    if entry["path"] == prefix or entry["path"].startswith(prefix + "/")]
        for entry in selected:
            path = entry["path"]
            if path in combined:
                raise ValueError(f"多个来源恢复为相同路径: {path}")
            combined[path] = entry
    return {"schema": 1, "toolVersion": TOOL_VERSION,
            "base": [str(path) for path in resolved], "generatedAt": now_iso(),
            "host": socket.gethostname(), "user": getpass.getuser(),
            "platform": platform.platform(),
            "entries": sorted(combined.values(), key=lambda item: item["path"].casefold())}


def save_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def markdown_escape(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def compare_manifests(source: dict, restored: dict, tolerance_ns: int) -> list[dict]:
    left = {entry["path"]: entry for entry in source["entries"]}
    right = {entry["path"]: entry for entry in restored["entries"]}
    results: list[dict] = []

    def add(status: str, category: str, path: str, detail: str) -> None:
        results.append({"status": status, "category": category, "path": path, "detail": detail})

    for path in sorted(set(left) | set(right), key=str.casefold):
        if path not in left:
            add("FAIL", "Path", path, "恢复目录出现来源中不存在的条目")
            continue
        if path not in right:
            add("FAIL", "Path", path, "恢复目录缺少条目")
            continue
        a, b = left[path], right[path]
        if a["kind"] != b["kind"]:
            add("FAIL", "Type", path, f"类型不同: {a['kind']} != {b['kind']}")
            continue
        add("PASS", "Path", path, "路径和类型一致")
        if a["kind"] == "file":
            add("PASS" if a.get("size") == b.get("size") else "FAIL", "Size", path,
                f"{a.get('size')} / {b.get('size')}")
            add("PASS" if a.get("sha256") == b.get("sha256") else "FAIL", "SHA-256", path,
                f"{a.get('sha256')} / {b.get('sha256')}")
        if a["kind"] == "directory" and a.get("empty"):
            add("PASS" if b.get("empty") else "FAIL", "Empty directory", path, "空目录应被保留")
        if a["kind"] in ("symlink", "junction", "reparse"):
            add("PASS" if a.get("target") == b.get("target") else "FAIL", "Link", path,
                f"{a.get('target')} / {b.get('target')}")
        difference = abs(int(a.get("mtimeNs", 0)) - int(b.get("mtimeNs", 0)))
        add("PASS" if difference <= tolerance_ns else "FAIL", "Mtime", path,
            f"差值 {difference / 1_000_000_000:.6f} 秒")
        add("PASS" if a.get("attributes") == b.get("attributes") else "FAIL", "Attributes", path,
            f"0x{a.get('attributes', 0):X} / 0x{b.get('attributes', 0):X}")
        if a.get("sddl") is None or b.get("sddl") is None:
            add("SKIP", "ACL", path, "无法读取一侧安全描述符")
        else:
            add("PASS" if a["sddl"] == b["sddl"] else "FAIL", "ACL", path,
                "SDDL 一致" if a["sddl"] == b["sddl"] else "owner/group/DACL 不同")
    return results


def git_commit() -> str:
    result = run(["git", "rev-parse", "--short", "HEAD"], cwd=repo_root())
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def write_report(path: Path, title: str, command: str, results: list[dict], evidence: list[str]) -> None:
    counts = {status: sum(1 for item in results if item["status"] == status)
              for status in ("PASS", "FAIL", "SKIP", "BLOCKED", "MANUAL", "WARN")}
    lines = [f"# {title}", "", f"- 生成时间：`{now_iso()}`", f"- 主机：`{socket.gethostname()}`",
             f"- 用户：`{getpass.getuser()}`", f"- 平台：`{platform.platform()}`",
             f"- Git commit：`{git_commit()}`", f"- 命令：`{command}`", "",
             "## 汇总", "", "| PASS | FAIL | SKIP | BLOCKED | MANUAL | WARN |", "|---:|---:|---:|---:|---:|---:|",
             f"| {counts['PASS']} | {counts['FAIL']} | {counts['SKIP']} | {counts['BLOCKED']} | {counts['MANUAL']} | {counts['WARN']} |",
             "", "## 明细", "", "| 状态 | 类别 | 路径 | 说明 |", "|---|---|---|---|"]
    for item in results:
        lines.append("| {status} | {category} | `{path}` | {detail} |".format(
            **{key: markdown_escape(value) for key, value in item.items()}))
    lines.extend(["", "## 证据", ""] + [f"- `{value}`" for value in evidence])
    lines.extend(["", "## 结论", "", "**FAIL**" if counts["FAIL"] else "**PASS（不含 SKIP/BLOCKED/MANUAL 项）**", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def command_manifest(args: argparse.Namespace) -> int:
    manifest = build_manifest(Path(args.input))
    save_json(Path(args.output), manifest)
    print(f"Manifest written: {Path(args.output).resolve()}")
    return EXIT_OK


def command_compare(args: argparse.Namespace) -> int:
    source = build_source_manifest(args.source)
    restored = build_manifest(Path(args.restored))
    report = Path(args.report).resolve()
    source_json = report.with_suffix(".source.json")
    restored_json = report.with_suffix(".restored.json")
    save_json(source_json, source)
    save_json(restored_json, restored)
    results = compare_manifests(source, restored, int(args.mtime_tolerance * 1_000_000_000))
    expected_roots = {entry["path"].split("/", 1)[0] for entry in source["entries"]}
    restored_roots = {entry["path"].split("/", 1)[0] for entry in restored["entries"]}
    if len(args.source) == 1 and restored_roots and restored_roots < expected_roots:
        results.insert(0, {"status": "FAIL", "category": "Source selection", "path": ".",
            "detail": "比较来源包含未备份的顶层目录。请为 GUI 中实际选择的每个来源分别传入 --source。"})
    write_report(report, "DBackup 恢复自动验收报告", " ".join(sys.argv), results,
                 [str(source_json), str(restored_json)])
    failures = sum(item["status"] == "FAIL" for item in results)
    print(f"Report: {report}")
    print(f"Checks: {len(results)}, failures: {failures}")
    return EXIT_FAILED if failures else EXIT_OK


def current_user_sid() -> str | None:
    result = run(["whoami", "/user", "/fo", "csv", "/nh"])
    if result.returncode:
        return None
    parts = [part.strip('"') for part in result.stdout.strip().split('","')]
    return parts[-1] if parts else None


def command_prepare_permissions(args: argparse.Namespace) -> int:
    root = normalize_root(args.root)
    load_marker(root)
    fixture = root / "fixtures/permissions"
    target = fixture / "denied-read.txt"
    backup = root / "evidence/permissions-acl.txt"
    if not target.exists():
        print("Permission fixture missing; run generate first.", file=sys.stderr)
        return EXIT_USAGE
    save = run(["icacls", str(target), "/save", str(backup), "/c"])
    sid = current_user_sid()
    if save.returncode or not sid:
        print(save.stdout + save.stderr, file=sys.stderr)
        return EXIT_BLOCKED
    deny = run(["icacls", str(target), "/inheritance:r", "/deny", f"*{sid}:(R)"])
    inherit = run(["icacls", str(fixture / "no-inheritance.txt"), "/inheritance:r"])
    if deny.returncode or inherit.returncode:
        print(deny.stdout + deny.stderr + inherit.stdout + inherit.stderr, file=sys.stderr)
        print("请在管理员终端重试；工具不会自动提权。", file=sys.stderr)
        return EXIT_BLOCKED
    print(f"Permission fixtures activated. ACL backup: {backup}")
    return EXIT_OK


def restore_permissions(root: Path) -> int:
    fixture = root / "fixtures/permissions"
    backup = root / "evidence/permissions-acl.txt"
    if backup.exists():
        restored = run(["icacls", str(fixture), "/restore", str(backup), "/c"])
        if restored.returncode:
            print(restored.stdout + restored.stderr, file=sys.stderr)
    reset = run(["icacls", str(fixture), "/reset", "/t", "/c"])
    for path in fixture.glob("*") if fixture.exists() else []:
        try:
            set_attributes(path, FILE_ATTRIBUTE_ARCHIVE)
        except OSError:
            pass
    return EXIT_OK if reset.returncode == 0 else EXIT_BLOCKED


def command_restore_permissions(args: argparse.Namespace) -> int:
    root = normalize_root(args.root)
    load_marker(root)
    code = restore_permissions(root)
    print("Permission fixtures restored." if code == 0 else "Permission reset needs administrator privileges.")
    return code


def command_prepare_links(args: argparse.Namespace) -> int:
    root = normalize_root(args.root)
    load_marker(root)
    links = root / "fixtures/links"
    links.mkdir(parents=True, exist_ok=True)
    (links / "target.txt").write_text("safe link target\n", encoding="utf-8")
    (links / "target-dir").mkdir(exist_ok=True)
    (links / "target-dir/inside.txt").write_text("inside\n", encoding="utf-8")
    outside = root / "outside-sentinel"
    outside.mkdir(exist_ok=True)
    (outside / "secret.txt").write_text("safe sentinel outside source fixture\n", encoding="utf-8")
    attempts = [
        ("file symlink", ["cmd", "/d", "/c", "mklink", str(links / "safe-file-link.txt"), "target.txt"]),
        ("directory symlink", ["cmd", "/d", "/c", "mklink", "/D", str(links / "safe-dir-link"), "target-dir"]),
        ("junction", ["cmd", "/d", "/c", "mklink", "/J", str(links / "safe-junction"), str(links / "target-dir")]),
        ("dangerous absolute link", ["cmd", "/d", "/c", "mklink", str(links / "dangerous-absolute.txt"), str(outside / "secret.txt")]),
    ]
    failed = []
    for label, command in attempts:
        destination = Path(command[-2])
        if destination.exists() or destination.is_symlink():
            continue
        result = run(command)
        if result.returncode:
            failed.append(f"{label}: {result.stdout}{result.stderr}".strip())
    save_json(root / "manifests/links-source.json", build_manifest(links))
    if failed:
        print("\n".join(failed), file=sys.stderr)
        print("符号链接需要 Windows 开发者模式或创建链接权限；Junction 可单独继续测试。", file=sys.stderr)
        return EXIT_BLOCKED
    print("Link fixtures created. Dangerous link targets only the acceptance sentinel.")
    return EXIT_OK


def mutate(root: Path, version: int) -> None:
    load_marker(root)
    base = root / "fixtures/incremental"
    if version == 2:
        large = base / "large-12m.bin"
        with large.open("r+b") as stream:
            stream.seek(5 * 1024**2 + 123)
            stream.write(b"DBackup-v2-small-change")
        (base / "new-in-v2.txt").write_text("new file\n", encoding="utf-8")
        (base / "delete-in-v2.txt").unlink(missing_ok=True)
        old, new = base / "rename-in-v2.txt", base / "renamed-in-v2.txt"
        if old.exists():
            old.rename(new)
    else:
        large = base / "large-12m.bin"
        with large.open("r+b") as stream:
            stream.seek(64)
            stream.write(b"first-block-change")
            stream.seek(-64, os.SEEK_END)
            stream.write(b"last-block-change")
        set_mtime(base / "mtime-only.txt", 3)
        set_attributes(base / "attributes-only.txt", FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY)
    save_json(root / f"manifests/incremental-v{version}.json", build_manifest(base))
    print(f"Incremental fixture mutated to version {version}.")


def command_mutate(args: argparse.Namespace) -> int:
    mutate(normalize_root(args.root), args.version)
    return EXIT_OK


def command_hold_pipe(args: argparse.Namespace) -> int:
    if os.name != "nt":
        return EXIT_BLOCKED
    root = normalize_root(args.root)
    marker = load_marker(root)
    name = rf"\\.\pipe\DBackupAcceptance-{marker['uuid']}"
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                        wintypes.DWORD, ctypes.c_void_p]
    kernel.CreateNamedPipeW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateNamedPipeW(name, 0x00000003, 0, 1, 4096, 4096, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        print(f"CreateNamedPipe failed: {ctypes.get_last_error()}", file=sys.stderr)
        return EXIT_BLOCKED
    print(f"Active named pipe: {name}")
    print("Press Ctrl+C to close it after the manual backup test.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        kernel.CloseHandle(handle)
        return EXIT_OK


def command_inspect_repository(args: argparse.Namespace) -> int:
    repository = Path(args.repository).resolve()
    files = [path for path in repository.rglob("*") if path.is_file()]
    chunks = [path for path in files if path.suffix.lower() == ".blk"]
    current = {"repository": str(repository), "generatedAt": now_iso(),
               "fileCount": len(files), "totalBytes": sum(path.stat().st_size for path in files),
               "chunkCount": len(chunks), "chunkBytes": sum(path.stat().st_size for path in chunks)}
    results = [{"status": "PASS", "category": "Repository", "path": str(repository),
                "detail": f"{len(chunks)} blocks, {current['chunkBytes']} bytes"}]
    evidence: list[str] = []
    if args.baseline:
        baseline_path = Path(args.baseline)
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        results.append({"status": "PASS", "category": "Delta", "path": str(repository),
                        "detail": f"新增块 {current['chunkCount'] - baseline['chunkCount']}，新增字节 {current['chunkBytes'] - baseline['chunkBytes']}"})
        evidence.append(str(baseline_path.resolve()))
    snapshot = Path(args.report).with_suffix(".repository.json")
    save_json(snapshot, current)
    evidence.append(str(snapshot.resolve()))
    write_report(Path(args.report), "DBackup 增量仓库统计报告", " ".join(sys.argv), results, evidence)
    print(f"Repository report: {Path(args.report).resolve()}")
    return EXIT_OK


def safe_cleanup(root: Path, confirmed: bool) -> None:
    marker = load_marker(root)
    if not confirmed:
        raise SafetyError("清理必须显式传入 --yes")
    normalize_root(str(root), require_drive=False)
    print(f"Cleaning marked acceptance root: {root}")
    print(f"Marker UUID: {marker['uuid']}")
    restore_permissions(root)
    for current, directories, files in os.walk(root, topdown=False, followlinks=False):
        for name in files + directories:
            path = Path(current) / name
            if get_attributes(path) & FILE_ATTRIBUTE_REPARSE_POINT:
                continue
            try:
                set_attributes(path, FILE_ATTRIBUTE_ARCHIVE if path.is_file() else 0x10)
                os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
            except OSError:
                pass
    shutil.rmtree(win_long(root))


def command_cleanup(args: argparse.Namespace) -> int:
    root = normalize_root(args.root)
    safe_cleanup(root, args.yes)
    print("Acceptance data removed.")
    return EXIT_OK


def command_generate(args: argparse.Namespace) -> int:
    generate_fixtures(normalize_root(args.root), args.profile, args.reset)
    return EXIT_OK


def command_self_test(_: argparse.Namespace) -> int:
    suite = unittest.defaultTestLoader.discover(str(Path(__file__).parent / "tests"), pattern="test_*.py")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return EXIT_OK if result.wasSuccessful() else EXIT_FAILED


def command_server_cert(args: argparse.Namespace) -> int:
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    openssl = shutil.which("openssl")
    if not openssl:
        raise FileNotFoundError("未找到 openssl；请安装 OpenSSL 或使用 Git for Windows 附带的 openssl")
    cert, key = output / "server-cert.pem", output / "server-key.pem"
    config = output / "openssl-test.cnf"
    config.write_text("[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=localhost\n", encoding="ascii")
    result = run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                  "-nodes", "-days", "30", "-config", str(config),
                  "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                  "-keyout", str(key), "-out", str(cert)])
    if result.returncode:
        print(result.stderr, file=sys.stderr)
        return EXIT_FAILED
    print(f"Certificate: {cert}\nPrivate key: {key}")
    return EXIT_OK


def command_server_smoke(args: argparse.Namespace) -> int:
    base = args.server.rstrip("/") + "/api/v1"
    username = f"acceptance-{uuid.uuid4().hex[:10]}"
    password = "DBackup-Acceptance-Only!2026"
    context = ssl.create_default_context(cafile=args.ca) if args.ca else ssl._create_unverified_context()
    results: list[tuple[str, bool, str]] = []

    def call(path: str, payload: dict | None = None, token: str = "", *, method: str | None = None,
             body: bytes | None = None) -> dict:
        data = body if body is not None else (json.dumps(payload).encode() if payload is not None else None)
        request = urllib.request.Request(base + path, data=data,
            headers={"Content-Type": "application/octet-stream" if body is not None else "application/json", **({"Authorization": "Bearer " + token} if token else {})},
            method=method or ("POST" if data is not None else "GET"))
        with urllib.request.urlopen(request, context=context, timeout=10) as response:
            raw = response.read()
            return {"_binary": raw} if response.headers.get_content_type() == "application/octet-stream" else json.loads(raw.decode("utf-8"))

    token = ""
    try:
        call("/register", {"username": username, "password": password}); results.append(("register", True, "201"))
        token = call("/login", {"username": username, "password": password})["token"]; results.append(("login", bool(token), "token received"))
        status = call("/status", token=token); results.append(("status", status.get("connected") is True, json.dumps(status)))
        chunk = b"DBackup server acceptance chunk"
        chunk_hash = hashlib.sha256(chunk).hexdigest()
        missing = call("/chunks/check", {"hashes": [chunk_hash]}, token)["missing"]
        results.append(("chunk-check", chunk_hash in missing, "missing chunk reported"))
        call("/chunks/" + chunk_hash, token=token, method="PUT", body=chunk)
        downloaded = call("/chunks/" + chunk_hash, token=token)["_binary"]
        results.append(("chunk-roundtrip", downloaded == chunk, "PUT/GET content matches"))
        snapshot_id = "acceptance-" + uuid.uuid4().hex
        call("/snapshots", {"id": snapshot_id, "name": "Acceptance", "chunks": [chunk_hash]}, token)
        snapshot = call("/snapshots/" + snapshot_id, token=token)
        results.append(("snapshot-commit", snapshot.get("id") == snapshot_id, "snapshot available"))
        snapshots = call("/snapshots", token=token); results.append(("snapshots", "snapshots" in snapshots, "list available"))
        call("/snapshots/" + snapshot_id, token=token, method="DELETE")
        results.append(("snapshot-delete", True, "snapshot deleted"))
    except (OSError, KeyError, ValueError, urllib.error.HTTPError) as exc:
        results.append(("request", False, str(exc)))
    report = Path(args.report).resolve(); report.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# DBackup Server Smoke Report", "", f"- Server: `{args.server}`", f"- Time: `{now_iso()}`", "", "| Check | Result | Detail |", "|---|---|---|"]
    for name, passed, detail in results:
        escaped_detail = detail.replace("|", "\\|")
        lines.append(f"| {name} | {'PASS' if passed else 'FAIL'} | {escaped_detail} |")
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(report)
    return EXIT_OK if results and all(item[1] for item in results) else EXIT_FAILED


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Independent DBackup acceptance fixture and comparison tool")
    sub = parser.add_subparsers(dest="command", required=True)
    generate = sub.add_parser("generate", help="Generate deterministic acceptance fixtures")
    generate.add_argument("--root")
    generate.add_argument("--profile", choices=("standard", "stress"), default="standard")
    generate.add_argument("--reset", action="store_true")
    generate.set_defaults(handler=command_generate)
    for name, handler in (("prepare-permissions", command_prepare_permissions),
                          ("restore-permissions", command_restore_permissions),
                          ("prepare-links", command_prepare_links),
                          ("hold-pipe", command_hold_pipe)):
        command = sub.add_parser(name)
        command.add_argument("--root")
        command.set_defaults(handler=handler)
    mutation = sub.add_parser("mutate")
    mutation.add_argument("--root")
    mutation.add_argument("--version", type=int, choices=(2, 3), required=True)
    mutation.set_defaults(handler=command_mutate)
    manifest = sub.add_parser("manifest")
    manifest.add_argument("--input", required=True)
    manifest.add_argument("--output", required=True)
    manifest.set_defaults(handler=command_manifest)
    compare = sub.add_parser("compare")
    compare.add_argument("--source", action="append", required=True,
                         help="Selected backup source; repeat for multiple files or directories")
    compare.add_argument("--restored", required=True)
    compare.add_argument("--report", required=True)
    compare.add_argument("--mtime-tolerance", type=float, default=2.0)
    compare.set_defaults(handler=command_compare)
    inspect = sub.add_parser("inspect-repository")
    inspect.add_argument("--repository", required=True)
    inspect.add_argument("--report", required=True)
    inspect.add_argument("--baseline")
    inspect.set_defaults(handler=command_inspect_repository)
    cleanup = sub.add_parser("cleanup")
    cleanup.add_argument("--root")
    cleanup.add_argument("--yes", action="store_true")
    cleanup.set_defaults(handler=command_cleanup)
    self_test = sub.add_parser("self-test")
    self_test.set_defaults(handler=command_self_test)
    certificate = sub.add_parser("server-cert", help="Generate a localhost acceptance certificate")
    certificate.add_argument("--output", required=True)
    certificate.set_defaults(handler=command_server_cert)
    smoke = sub.add_parser("server-smoke", help="Exercise the HTTPS authentication and listing API")
    smoke.add_argument("--server", default="https://127.0.0.1:8443")
    smoke.add_argument("--ca")
    smoke.add_argument("--report", required=True)
    smoke.set_defaults(handler=command_server_smoke)
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        args = build_parser().parse_args(argv)
        return int(args.handler(args))
    except SafetyError as exc:
        print(f"SAFETY: {exc}", file=sys.stderr)
        return EXIT_SAFETY
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except PermissionError as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        return EXIT_BLOCKED
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return EXIT_BLOCKED


if __name__ == "__main__":
    raise SystemExit(main())

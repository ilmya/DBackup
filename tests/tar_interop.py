#!/usr/bin/env python3
"""Cross-check plain POSIX pax tar with the system tar implementation."""
import argparse
import io
import os
import stat
from pathlib import Path
import subprocess
import sys
import tempfile
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "acceptance"))
import dbackup_acceptance as tool


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    tool.load_marker(root)
    with tempfile.TemporaryDirectory(prefix="tar-interop-", dir=root) as directory:
        base = Path(directory)
        source = base / "source"
        source.mkdir()
        nested = source / ("long-" + "a" * 110)
        nested.mkdir()
        (nested / "file.txt").write_text("tar interoperability\n")
        os.link(nested / "file.txt", source / "hardlink")
        os.symlink("hardlink", source / "symlink")
        os.mkfifo(source / "pipe", 0o640)
        timestamp = 1700000000123456789
        for path in sorted(source.rglob("*"), key=lambda p: len(p.parts), reverse=True):
            os.utime(path, ns=(timestamp, timestamp), follow_symlinks=False)
        dbackup_tar = base / "dbackup.tar"
        system_tar = base / "system.tar"
        system_restore = base / "system-restore"
        system_restore.mkdir()
        dbackup_restore = base / "dbackup-restore"
        common = [args.cli, "archive"]
        subprocess.run([*common, "create", "--root", str(root), "--source", str(source),
                        "--output", str(dbackup_tar), "--format", "tar", "--algorithm", "none"], check=True)
        subprocess.run(["tar", "-xf", str(dbackup_tar), "-C", str(system_restore)], check=True)
        subprocess.run(["tar", "--format=pax", "-cf", str(system_tar), "-C", str(source), "."], check=True)
        subprocess.run([*common, "extract", "--root", str(root), "--input", str(system_tar),
                        "--output", str(dbackup_restore)], check=True)
        reference = tool.build_manifest(source)
        for restored in (system_restore, dbackup_restore):
            results = tool.compare_manifests(reference, tool.build_manifest(restored), 0)
            failures = [r for r in results if r["status"] == "FAIL"]
            if failures:
                raise RuntimeError(f"{restored}: {failures}")
            print(f"{restored}: {len(results)} checks passed")
        socket_source = base / "socket-source"
        socket_source.mkdir()
        os.mknod(socket_source / "endpoint", stat.S_IFSOCK | 0o600)
        socket_archive = base / "socket.tar"
        subprocess.run([*common, "create", "--root", str(root), "--source", str(socket_source),
                        "--output", str(socket_archive), "--format", "tar", "--algorithm", "none"],
                       check=True, capture_output=True)
        with tarfile.open(socket_archive) as tar:
            member = tar.getmember("endpoint")
            if member.pax_headers.get("DBackup.type") != "socket":
                raise RuntimeError("socket PAX extension missing")
        external_socket = base / "external-socket"
        external_socket.mkdir()
        subprocess.run(["tar", "-xf", str(socket_archive), "-C", str(external_socket)],
                       check=True, capture_output=True)
        if not (external_socket / "endpoint").is_file():
            raise RuntimeError("ordinary tar socket placeholder behavior changed")
        for pack in ("custom", "tar"):
            for algorithm in ("none", "huffman", "lz77", "deflate"):
                for encrypted in (False, True):
                    label = f"{pack}-{algorithm}-{'encrypted' if encrypted else 'plain'}"
                    archive, restored = base / (label + ".abk"), base / (label + "-restore")
                    env = {**os.environ, "DBACKUP_TEST_PASSWORD": "demo-only" if encrypted else ""}
                    subprocess.run([*common, "create", "--root", str(root), "--source", str(source),
                                    "--output", str(archive), "--format", pack, "--algorithm", algorithm],
                                   env=env, check=True, capture_output=True)
                    if encrypted:
                        wrong = base / (label + "-wrong")
                        result = subprocess.run([*common, "extract", "--root", str(root), "--input", str(archive),
                                                 "--output", str(wrong)], env={**env, "DBACKUP_TEST_PASSWORD": "wrong"},
                                                capture_output=True)
                        if result.returncode == 0 or wrong.exists():
                            raise RuntimeError(f"wrong password accepted: {label}")
                    subprocess.run([*common, "extract", "--root", str(root), "--input", str(archive),
                                    "--output", str(restored)], env=env, check=True, capture_output=True)
                    failures = [r for r in tool.compare_manifests(reference, tool.build_manifest(restored), 0)
                                if r["status"] == "FAIL"]
                    if failures:
                        raise RuntimeError(f"{label}: {failures}")
                    print(f"{label}: special files and metadata passed")
        for name, target in (("escape", "../outside"), ("absolute", "/tmp/dbackup-outside")):
            archive = base / (name + ".tar")
            with tarfile.open(archive, "w", format=tarfile.PAX_FORMAT) as tar:
                entry = tarfile.TarInfo(target)
                entry.size = 1
                tar.addfile(entry, io.BytesIO(b"x"))
            destination = base / (name + "-restore")
            result = subprocess.run([*common, "extract", "--root", str(root), "--input", str(archive),
                                     "--output", str(destination)], capture_output=True, text=True)
            if result.returncode == 0 or destination.exists():
                raise RuntimeError(f"unsafe tar path accepted: {target}")
        device_tar = base / "device.tar"
        with tarfile.open(device_tar, "w", format=tarfile.PAX_FORMAT) as tar:
            entry = tarfile.TarInfo("device")
            entry.type = tarfile.CHRTYPE
            entry.devmajor, entry.devminor = 1, 3
            tar.addfile(entry)
        destination = base / "device-restore"
        result = subprocess.run([*common, "extract", "--root", str(root), "--input", str(device_tar),
                                 "--output", str(destination), "--skip-device-nodes"], capture_output=True, text=True)
        if result.returncode != 0 or "设备节点" not in result.stdout or (destination / "device").exists():
            raise RuntimeError("disabled device node was not skipped with a warning")


if __name__ == "__main__":
    main()

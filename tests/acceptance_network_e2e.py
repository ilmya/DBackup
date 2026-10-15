#!/usr/bin/env python3
"""Start an isolated HTTPS server and exercise the production Qt remote client."""

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
import ssl

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "acceptance"))
import dbackup_acceptance as tool  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--server-exe", required=True)
    parser.add_argument("--test-cli", required=True)
    parser.add_argument("--openssl", required=True)
    args = parser.parse_args()

    root = tool.normalize_root(args.root)
    tool.load_marker(root)
    server_exe = Path(args.server_exe).resolve()
    test_cli = Path(args.test_cli).resolve()
    openssl = Path(args.openssl).resolve()
    if not server_exe.is_file() or not test_cli.is_file() or not openssl.is_file():
        raise FileNotFoundError("DBackupServer.exe、BackupToolTestCli.exe 或 openssl.exe 不存在")

    work = root / "server-data" / ("e2e-" + uuid.uuid4().hex)
    work.mkdir(parents=True)
    cert, key = work / "server-cert.pem", work / "server-key.pem"
    config = work / "openssl-test.cnf"
    config.write_text("[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=localhost\n", encoding="ascii")
    generated = subprocess.run([
        str(openssl), "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "2",
        "-config", str(config), "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(key), "-out", str(cert),
    ], cwd=work, capture_output=True, text=True, timeout=45)
    if generated.returncode != 0:
        print(generated.stdout + generated.stderr, file=sys.stderr)
        return 1

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    endpoint = f"https://127.0.0.1:{port}"
    log_path = work / "server.log"
    env = os.environ.copy()
    with log_path.open("wb") as log:
        server = subprocess.Popen([
            str(server_exe), "--data", str(work / "data"), "--cert", str(cert), "--key", str(key),
            "--listen", "127.0.0.1", "--port", str(port),
        ], cwd=server_exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            context = ssl._create_unverified_context()
            ready = False
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and server.poll() is None:
                try:
                    urllib.request.urlopen(endpoint + "/api/v1/status", context=context, timeout=1)
                except urllib.error.HTTPError as exc:
                    if exc.code == 401:
                        ready = True
                        break
                except OSError:
                    pass
                time.sleep(0.2)
            if not ready:
                print("DBackupServer did not become ready:\n" + log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
                return 1

            result = subprocess.run([
                str(test_cli), "remote-client-test", "--root", str(root), "--server", endpoint,
            ], cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
            if result.stdout:
                print(result.stdout, end="")
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="")
            return result.returncode
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)

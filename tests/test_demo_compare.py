#!/usr/bin/env python3
# 验证演示数据与恢复结果的比较
"""Verify that demonstration comparisons catch special-type and link differences."""
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "acceptance"))
import dbackup_acceptance as tool


class DemoComparisonTest(unittest.TestCase):
    def test_detects_broken_hardlink_and_fifo(self):
        with tempfile.TemporaryDirectory(prefix="dbackup-demo-compare-") as temporary:
            root = Path(temporary)
            left, right = root / "left", root / "right"
            left.mkdir()
            right.mkdir()
            for folder in (left, right):
                (folder / "a").write_bytes(b"same content")
                os.link(folder / "a", folder / "b")
                os.mkfifo(folder / "pipe")
            source = tool.build_manifest(left)
            (right / "b").unlink()
            (right / "b").write_bytes(b"same content")
            (right / "pipe").unlink()
            (right / "pipe").touch()
            results = tool.compare_manifests(source, tool.build_manifest(right), 10**18)
            self.assertTrue(any(r["status"] == "FAIL" and r["category"] == "Hardlink" for r in results))
            self.assertTrue(any(r["status"] == "FAIL" and r["category"] == "Type" and r["path"] == "pipe" for r in results))


if __name__ == "__main__":
    unittest.main()

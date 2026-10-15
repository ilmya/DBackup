#!/usr/bin/env python3
"""Run the independent acceptance-tool unit tests in a pre-created work area."""

import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = Path(os.environ["DBACKUP_ACCEPTANCE_TEST_TMP"]).resolve()
TEST_DIR.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / "tools" / "acceptance" / "tests"))
import test_acceptance_tool  # noqa: E402


def clear_directory(path: Path) -> None:
    for item in path.iterdir():
        if item.is_symlink() or item.is_file():
            item.unlink()
        elif item.is_dir():
            shutil.rmtree(item)


original_rmtree = shutil.rmtree
original_mkdtemp = tempfile.mkdtemp


def stable_mkdtemp(*args, **kwargs):
    clear_directory(TEST_DIR)
    return str(TEST_DIR)


def preserve_test_root(path, *args, **kwargs):
    try:
        target = Path(path).resolve()
    except (OSError, TypeError):
        return original_rmtree(path, *args, **kwargs)
    if target == TEST_DIR:
        clear_directory(TEST_DIR)
        return
    return original_rmtree(path, *args, **kwargs)


tempfile.mkdtemp = stable_mkdtemp
shutil.rmtree = preserve_test_root
suite = unittest.defaultTestLoader.loadTestsFromModule(test_acceptance_tool)
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)

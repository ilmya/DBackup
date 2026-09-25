import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dbackup_acceptance as tool


class AcceptanceToolTest(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="dbackup-acceptance-tool-"))

    def tearDown(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def test_deterministic_stream_and_hash(self):
        first, second = self.temp / "first.bin", self.temp / "second.bin"
        tool.write_pattern(first, 1024 * 1024 + 7, "random", 42)
        tool.write_pattern(second, 1024 * 1024 + 7, "random", 42)
        self.assertEqual(tool.sha256_file(first), tool.sha256_file(second))

    def test_identical_tree_passes_with_empty_directory(self):
        source, restored = self.temp / "source", self.temp / "restored"
        (source / "empty").mkdir(parents=True)
        (source / "file.txt").write_text("same", encoding="utf-8")
        shutil.copytree(source, restored)
        results = tool.compare_manifests(tool.build_manifest(source), tool.build_manifest(restored), 2_000_000_000)
        self.assertFalse([item for item in results if item["status"] == "FAIL"])
        self.assertTrue(any(item["category"] == "Empty directory" for item in results))

    def test_multiple_selected_sources_match_restored_root(self):
        fixtures, restored = self.temp / "fixtures", self.temp / "restored"
        (fixtures / "normal").mkdir(parents=True)
        (fixtures / "compression").mkdir()
        (fixtures / "not-selected").mkdir()
        (fixtures / "normal/a.txt").write_text("a", encoding="utf-8")
        (fixtures / "compression/b.txt").write_text("b", encoding="utf-8")
        shutil.copytree(fixtures / "normal", restored / "normal")
        shutil.copytree(fixtures / "compression", restored / "compression")
        expected = tool.build_source_manifest([str(fixtures / "normal"), str(fixtures / "compression")])
        results = tool.compare_manifests(expected, tool.build_manifest(restored), 2_000_000_000)
        self.assertFalse([item for item in results if item["status"] == "FAIL"])
        self.assertNotIn("not-selected", {item["path"] for item in expected["entries"]})

    def test_missing_extra_and_corrupt_files_are_reported(self):
        source, restored = self.temp / "source", self.temp / "restored"
        source.mkdir(); restored.mkdir()
        (source / "missing.txt").write_text("missing", encoding="utf-8")
        (source / "corrupt.txt").write_text("original", encoding="utf-8")
        (restored / "corrupt.txt").write_text("changed", encoding="utf-8")
        (restored / "extra.txt").write_text("extra", encoding="utf-8")
        results = tool.compare_manifests(tool.build_manifest(source), tool.build_manifest(restored), 2_000_000_000)
        failures = {(item["category"], item["path"]) for item in results if item["status"] == "FAIL"}
        self.assertIn(("Path", "missing.txt"), failures)
        self.assertIn(("Path", "extra.txt"), failures)
        self.assertIn(("SHA-256", "corrupt.txt"), failures)

    def test_mtime_tolerance(self):
        source, restored = self.temp / "source", self.temp / "restored"
        source.mkdir(); restored.mkdir()
        a, b = source / "time.txt", restored / "time.txt"
        a.write_text("same", encoding="utf-8"); b.write_text("same", encoding="utf-8")
        stamp = 1_700_000_000
        os.utime(a, (stamp, stamp)); os.utime(b, (stamp + 1, stamp + 1))
        results = tool.compare_manifests(tool.build_manifest(source), tool.build_manifest(restored), 2_000_000_000)
        mtime = [item for item in results if item["category"] == "Mtime"]
        self.assertEqual(mtime[0]["status"], "PASS")

    def test_markdown_escape(self):
        self.assertEqual(tool.markdown_escape("a|b\nc"), "a\\|b c")

    def test_cleanup_refuses_unmarked_directory(self):
        root = self.temp / "unmarked"
        root.mkdir()
        with self.assertRaises(tool.SafetyError):
            tool.safe_cleanup(root, confirmed=True)
        self.assertTrue(root.exists())

    def test_cleanup_requires_confirmation(self):
        root = self.temp / "marked"
        root.mkdir()
        tool.write_marker(root)
        with self.assertRaises(tool.SafetyError):
            tool.safe_cleanup(root, confirmed=False)
        self.assertTrue(root.exists())

    def test_invalid_marker_is_rejected(self):
        root = self.temp / "invalid"
        root.mkdir()
        (root / tool.MARKER).write_text(json.dumps({"kind": "something-else"}), encoding="utf-8")
        with self.assertRaises(tool.SafetyError):
            tool.load_marker(root)

    def test_drive_root_is_rejected(self):
        with self.assertRaises(tool.SafetyError):
            tool.normalize_root(Path.cwd().anchor, require_drive=False)

    @unittest.skipUnless(os.name == "nt", "Windows attributes")
    def test_attribute_difference_is_reported(self):
        source, restored = self.temp / "source", self.temp / "restored"
        source.mkdir(); restored.mkdir()
        (source / "item.txt").write_text("same", encoding="utf-8")
        (restored / "item.txt").write_text("same", encoding="utf-8")
        tool.set_attributes(source / "item.txt", tool.FILE_ATTRIBUTE_ARCHIVE | tool.FILE_ATTRIBUTE_HIDDEN)
        results = tool.compare_manifests(tool.build_manifest(source), tool.build_manifest(restored), 2_000_000_000)
        attributes = [item for item in results if item["category"] == "Attributes"]
        self.assertEqual(attributes[0]["status"], "FAIL")

    @unittest.skipUnless(os.name == "nt", "Windows link semantics")
    def test_symlink_is_not_followed_when_available(self):
        root = self.temp / "links"
        root.mkdir()
        target = root / "target.txt"
        link = root / "link.txt"
        target.write_text("target", encoding="utf-8")
        try:
            link.symlink_to("target.txt")
        except OSError as exc:
            self.skipTest(f"symlink privilege unavailable: {exc}")
        manifest = tool.build_manifest(root)
        entry = next(item for item in manifest["entries"] if item["path"] == "link.txt")
        self.assertEqual(entry["kind"], "symlink")
        self.assertEqual(entry["target"], "target.txt")


if __name__ == "__main__":
    unittest.main()

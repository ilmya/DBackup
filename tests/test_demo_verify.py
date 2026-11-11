#!/usr/bin/env python3
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import demo_verify as verify


class DemoVerifyTest(unittest.TestCase):
    def test_regular_file_bytes_counts_links_without_following_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'file').write_bytes(b'abc')
            os.link(root / 'file', root / 'hardlink')
            (root / 'symlink').symlink_to(root / 'file')
            self.assertEqual(verify.regular_file_bytes(root), 6)

    def test_arguments_and_defaults(self):
        options = verify.parser().parse_args(['--format', 'tar', '--compression', 'none', '--encrypt', '--group', 'filters'])
        self.assertEqual((options.format, options.compression, options.encrypt, options.group),
                         ('tar', 'none', True, ['filters']))
        self.assertEqual(verify.parser().parse_args([]).group, None)
        with self.assertRaises(SystemExit):
            verify.parser().parse_args(['--compression', 'invalid'])

    def test_exit_status_distinguishes_expected_skip_and_failure(self):
        skipped = [verify.result('SKIP', 'device', 'node', 'expected')]
        self.assertEqual(verify.exit_status(skipped, False), 0)
        self.assertEqual(verify.exit_status(skipped, True), 2)
        self.assertEqual(verify.exit_status([verify.result('BLOCKED', 'node', 'x', '')], False), 2)
        failed = skipped + [verify.result('FAIL', 'path', 'missing', 'unexpected')]
        self.assertEqual(verify.exit_status(failed, False), 1)
        self.assertEqual(verify.exit_status(failed, True), 1)

    def test_runs_create_separate_result_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'restore').mkdir()
            options = verify.parser().parse_args(['--group', 'filters'])
            with patch.object(verify.demo, 'validate_layout'), \
                 patch.object(verify, 'execute', return_value=({'ok': False, 'error': 'expected failure'}, 0)):
                self.assertEqual(verify.run(root, Path('/unused-cli'), options), 1)
                self.assertEqual(verify.run(root, Path('/unused-cli'), options), 1)
            dirs = list((root / 'restore').iterdir())
            self.assertEqual(len(dirs), 2)
            self.assertTrue(all((p / 'summary.md').is_file() for p in dirs))

    def test_expected_device_skip_does_not_hide_unexpected_missing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, restored = root / 'src', root / 'dst'
            source.mkdir(); restored.mkdir()
            (source / 'missing').write_text('missing')
            device = {'path': 'node', 'kind': 'char_device', 'major': 1, 'minor': 3,
                      'uid': 0, 'gid': 0, 'mtimeNs': 0, 'attributes': 0}
            original = verify.tool.build_manifest
            def manifest(path):
                value = original(path)
                if path == source:
                    value['entries'].append(device)
                return value
            with patch.object(verify.tool, 'build_manifest', side_effect=manifest):
                rows = verify.compare(source, restored, False, '', 'node: Operation not permitted')
            self.assertTrue(any(r['status'] == 'SKIP' and r['path'] == 'node' for r in rows))
            self.assertTrue(any(r['status'] == 'FAIL' and r['path'] == 'missing' for r in rows))

    def test_device_not_skipped_without_recorded_warning(self):
        expected = {'schema': 2, 'entries': [{'path': 'node', 'kind': 'char_device'}]}
        actual = {'schema': 2, 'entries': []}
        with patch.object(verify.tool, 'build_manifest', side_effect=[expected, actual]):
            rows = verify.compare(Path('/src'), Path('/dst'), False, '', '')
        self.assertEqual(rows[0]['status'], 'FAIL')

    def test_privileged_permission_failure_is_blocked(self):
        expected = {'schema': 2, 'entries': [{'path': 'node', 'kind': 'char_device'}]}
        actual = {'schema': 2, 'entries': []}
        with patch.object(verify.tool, 'build_manifest', side_effect=[expected, actual]):
            rows = verify.compare(Path('/src'), Path('/dst'), True, '', 'node: Operation not permitted')
        self.assertEqual(rows[0]['status'], 'BLOCKED')
        self.assertEqual(verify.exit_status(rows, True), 2)

    def test_restore_target_rejects_escape_and_symlink(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / 'restore').mkdir()
            outside = root / 'outside'; outside.mkdir()
            (root / 'restore' / 'link').symlink_to(outside, target_is_directory=True)
            with self.assertRaises(verify.tool.SafetyError):
                verify.demo.validate_restore_target(root, str(outside))
            with self.assertRaises(verify.tool.SafetyError):
                verify.demo.validate_restore_target(root, str(root / 'restore' / 'link'))

    def test_failed_command_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as temp:
            fake = Path(temp) / 'cli'
            fake.write_text('#!/bin/sh\necho \'{"ok":false,"error":"wrong password"}\'\nexit 1\n')
            fake.chmod(0o700)
            value, _ = verify.execute(fake, [], 'test-only', 256)
            self.assertFalse(value['ok'])
            self.assertIn('wrong password', value['error'])


if __name__ == '__main__':
    unittest.main()

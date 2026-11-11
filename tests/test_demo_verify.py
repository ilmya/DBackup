#!/usr/bin/env python3
import os
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import demo_verify as verify


class DemoVerifyTest(unittest.TestCase):
    def test_clear_restore_preserves_sources_and_symlink_targets(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            root = base / 'demo'
            root.mkdir()
            for name in ('common', 'large-data', 'special-files', 'restore'):
                (root / name).mkdir()
            marker = verify.tool.write_marker(root)
            marker['demoLayoutVersion'] = verify.demo.LAYOUT_VERSION
            (root / verify.demo.MARKER).write_text(json.dumps(marker), encoding='utf-8')
            (root / 'common' / 'source').write_text('keep')
            outside = base / 'outside'
            outside.mkdir()
            (outside / 'sentinel').write_text('keep')
            (root / 'restore' / 'nested').mkdir()
            (root / 'restore' / 'nested' / 'result').write_text('delete')
            (root / 'restore' / '.hidden').write_text('delete')
            (root / 'restore' / 'link').symlink_to(outside, target_is_directory=True)
            self.assertEqual(verify.demo.clear_restore(root), 3)
            self.assertEqual(list((root / 'restore').iterdir()), [])
            self.assertEqual((root / 'common' / 'source').read_text(), 'keep')
            self.assertEqual((outside / 'sentinel').read_text(), 'keep')

    def test_clear_restore_rejects_symlinked_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            root = base / 'demo'
            root.mkdir()
            for name in ('common', 'large-data', 'special-files'):
                (root / name).mkdir()
            outside = base / 'outside'
            outside.mkdir()
            (outside / 'sentinel').write_text('keep')
            (root / 'restore').symlink_to(outside, target_is_directory=True)
            marker = verify.tool.write_marker(root)
            marker['demoLayoutVersion'] = verify.demo.LAYOUT_VERSION
            (root / verify.demo.MARKER).write_text(json.dumps(marker), encoding='utf-8')
            with self.assertRaises(verify.tool.SafetyError):
                verify.demo.clear_restore(root)
            self.assertEqual((outside / 'sentinel').read_text(), 'keep')

    def test_clear_restore_rejects_nested_mount(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'restore').mkdir()
            (root / 'restore' / 'keep').write_text('keep')
            mountinfo = f'1 0 0:1 / {root}/restore/mounted rw - tmpfs tmpfs rw\n'
            with patch.object(verify.demo, 'validate_layout'), \
                 patch.object(verify.demo.Path, 'read_text', return_value=mountinfo):
                with self.assertRaises(verify.tool.SafetyError):
                    verify.demo.clear_restore(root)
            self.assertEqual((root / 'restore' / 'keep').read_text(), 'keep')

    def test_console_details_include_fail_skip_and_warning_but_not_pass(self):
        rows = [verify.result('PASS', 'hash', 'ok', 'same'),
                verify.result('FAIL', '备份', 'large-data', '磁盘空间不足\n请检查空间'),
                verify.result('SKIP', '设备节点', 'nodes/device', '权限不足'),
                verify.result('WARN', '恢复警告', 'socket', '服务未恢复'),
                verify.result('WARN', '恢复警告', 'socket', '套接字节点不包含原服务或连接，可能占用服务路径')]
        output = io.StringIO()
        with redirect_stdout(output):
            verify.print_details(rows)
        text = output.getvalue()
        self.assertIn('[失败] 备份 · 大文件：磁盘空间不足', text)
        self.assertIn('请检查空间', text)
        self.assertIn('[跳过] 设备节点 · nodes/device：权限不足', text)
        self.assertIn('[警告] 恢复警告 · socket：服务未恢复', text)
        self.assertIn('[提示] 恢复提示 · socket：套接字节点不包含原服务或连接', text)
        self.assertNotIn('[通过]', text)

    def test_cli_failure_hides_progress_but_keeps_signal_and_diagnostics(self):
        proc = subprocess.CompletedProcess([], -6, '',
            '{"stage":"reading","path":"file"}\nterminate called after throwing std::bad_alloc\n')
        detail = verify.cli_failure(proc)
        self.assertIn('SIGABRT', detail)
        self.assertIn('std::bad_alloc', detail)
        self.assertIn('bash build.sh test', detail)
        self.assertNotIn('"stage"', detail)

    def test_regular_file_bytes_counts_links_without_following_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'file').write_bytes(b'abc')
            os.link(root / 'file', root / 'hardlink')
            (root / 'symlink').symlink_to(root / 'file')
            self.assertEqual(verify.regular_file_bytes(root), 6)

    def test_arguments_and_defaults(self):
        options = verify.parser().parse_args(['--format', 'tar', '--compression', 'none', '--encrypt', '--group', 'common'])
        self.assertEqual((options.format, options.compression, options.encrypt, options.group),
                         ('tar', 'none', True, ['common']))
        defaults = verify.parser().parse_args([])
        self.assertIsNone(defaults.group)
        self.assertTrue(defaults.encrypt)
        self.assertEqual(verify.TEST_PASSWORD, '123')
        self.assertFalse(verify.parser().parse_args(['--no-encrypt']).encrypt)
        with self.assertRaises(SystemExit):
            verify.parser().parse_args(['--compression', 'invalid'])
        with self.assertRaises(SystemExit):
            verify.parser().parse_args(['--memory-mib', '1024'])

    def test_exit_status_distinguishes_expected_skip_and_failure(self):
        skipped = [verify.result('SKIP', 'device', 'node', 'expected')]
        self.assertEqual(verify.exit_status(skipped, False), 0)
        self.assertEqual(verify.exit_status(skipped, True), 2)
        self.assertEqual(verify.exit_status([verify.result('BLOCKED', 'node', 'x', '')], False), 2)
        failed = skipped + [verify.result('FAIL', 'path', 'missing', 'unexpected')]
        self.assertEqual(verify.exit_status(failed, False), 1)
        self.assertEqual(verify.exit_status(failed, True), 1)

    def test_default_encryption_uses_fixed_demo_password(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'restore').mkdir()
            options = verify.parser().parse_args(['--group', 'common'])
            with patch.dict(os.environ, {'DBACKUP_TEST_PASSWORD': 'outside-value'}), \
                 patch.object(verify.demo, 'validate_layout'), \
                 patch.object(verify, 'execute', return_value=({'ok': False, 'error': 'expected'}, 0)) as execute:
                self.assertEqual(verify.run(root, Path('/unused-cli'), options), 1)
            self.assertEqual(execute.call_args.args[2], '123')

    def test_runs_create_separate_result_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'restore').mkdir()
            options = verify.parser().parse_args(['--group', 'common'])
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
            value, _ = verify.execute(fake, [], 'test-only')
            self.assertFalse(value['ok'])
            self.assertIn('wrong password', value['error'])


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# 执行演示流程并核对备份与恢复结果
"""Run backup, restore and comparison against the existing WSL demo corpus."""
import argparse
import errno
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import sys
import tempfile
import time

import demo

tool = demo.tool
TEST_PASSWORD = '123'
GROUPS = ('common', 'large-data', 'special-files')
GROUP_LABELS = {'common': '常规筛选', 'large-data': '大文件', 'special-files': '特殊文件'}
FORMAT_LABELS = {'custom': '自定义', 'tar': 'tar'}
COMPRESSION_LABELS = {'none': '不压缩', 'huffman': 'Huffman', 'lz77': 'LZ77',
                      'deflate': 'DEFLATE'}
STATUS_LABELS = {'PASS': '通过', 'PARTIAL': '部分完成', 'FAIL': '失败',
                 'SKIP': '跳过', 'BLOCKED': '受阻', 'WARN': '警告'}
CATEGORY_LABELS = {'Path': '路径', 'Type': '类型', 'Size': '大小', 'Hardlink': '硬链接',
                   'Device': '设备', 'Empty directory': '空目录', 'Link': '链接',
                   'Mtime': '修改时间', 'Attributes': '权限', 'Ownership': '属主'}


def regular_file_bytes(root):
    """Count logical regular-file bytes; hard-linked paths are counted separately."""
    total = 0
    for directory, _, files in os.walk(root, followlinks=False):
        for name in files:
            try:
                info = os.lstat(Path(directory) / name)
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--format', choices=('custom', 'tar'), default='custom')
    p.add_argument('--compression', choices=('none', 'huffman', 'lz77', 'deflate'), default='deflate')
    encryption = p.add_mutually_exclusive_group()
    encryption.add_argument('--encrypt', dest='encrypt', action='store_true',
                            help='启用加密（默认，演示密码为 123）')
    encryption.add_argument('--no-encrypt', dest='encrypt', action='store_false',
                            help='不加密')
    p.set_defaults(encrypt=True)
    p.add_argument('--group', choices=GROUPS, action='append', help='可重复；默认运行全部三组')
    return p


def result(status, category, path, detail):
    return {'status': status, 'category': category, 'path': str(path), 'detail': str(detail)}


def exit_status(rows, privileged):
    if any(row['status'] == 'FAIL' for row in rows):
        return 1
    if any(row['status'] == 'BLOCKED' for row in rows) or (privileged and any(row['status'] == 'SKIP' for row in rows)):
        return 2
    return 0


def permission_warning(warnings, path):
    return any(path in line and (os.strerror(errno.EPERM) in line or os.strerror(errno.EACCES) in line)
               for line in warnings.replace('\\n', '\n').splitlines())


def cli_failure(proc):
    """Keep actual diagnostics while suppressing the CLI's JSON progress events."""
    diagnostics = []
    for line in proc.stderr.splitlines():
        try:
            event = json.loads(line)
        except (ValueError, TypeError):
            event = None
        if isinstance(event, dict) and 'stage' in event:
            continue
        if line.strip():
            diagnostics.append(line.strip())
    if proc.returncode < 0:
        try:
            status = f'CLI 被信号 {signal.Signals(-proc.returncode).name} 终止（退出码 {proc.returncode}）'
        except ValueError:
            status = f'CLI 被信号 {-proc.returncode} 终止（退出码 {proc.returncode}）'
    else:
        status = f'CLI 退出码 {proc.returncode}'
    detail = '\n'.join(diagnostics)[-1000:]
    if detail:
        status += f'：{detail}'
    if 'std::bad_alloc' in detail:
        status += '\n提示：测试程序内存分配失败；请确认已用 bash build.sh test 更新测试程序，并检查系统可用内存。'
    return status


def print_details(rows):
    """Show actionable comparison results; complete PASS details remain in the report."""
    for row in rows:
        if row['status'] not in ('FAIL', 'SKIP', 'BLOCKED', 'WARN'):
            continue
        detail = str(row['detail']).replace('\\n', '\n')
        detail = detail.replace('Permission denied', '权限不足').replace(
            'Operation not permitted', '无权执行此操作')
        detail = detail.replace('[Errno 13] ', '').replace('[Errno 1] ', '')
        lines = detail.splitlines() or ['']
        category = CATEGORY_LABELS.get(row['category'], row['category'])
        label = ('提示' if row['status'] == 'WARN' and
                 '套接字节点不包含原服务或连接' in detail else STATUS_LABELS[row['status']])
        if label == '提示' and category == '恢复警告':
            category = '恢复提示'
        path = GROUP_LABELS.get(row['path'], row['path'])
        print(f"  [{label}] {category} · {path}：{lines[0]}")
        for line in lines[1:]:
            print(f'    {line}')


def compare(source, restored, privileged, backup_warning, restore_warning):
    expected = tool.build_manifest(source)
    actual = tool.build_manifest(restored)
    actual_by_path = {entry['path']: entry for entry in actual['entries']}
    skipped = []
    kept = []
    for entry in expected['entries']:
        name = entry['path']
        if entry['kind'] == 'unreadable' and permission_warning(backup_warning, name):
            skipped.append(result('BLOCKED' if privileged else 'SKIP', '不可读来源', name, entry.get('error', '权限不足')))
        elif (entry['kind'] in ('char_device', 'block_device')
              and name not in actual_by_path and permission_warning(restore_warning, name)):
            skipped.append(result('BLOCKED' if privileged else 'SKIP', '设备节点', name, '程序尝试恢复后因权限不足跳过'))
        else:
            kept.append(entry)
    expected['entries'] = kept
    comparisons = tool.compare_manifests(expected, actual, 0)
    left = {e['path']: e for e in kept}
    for item in comparisons:
        if (item['category'] == 'Ownership' and item['status'] == 'FAIL'
                and permission_warning(restore_warning, item['path']) and item['path'] in actual_by_path
                and actual_by_path[item['path']]['uid'] == os.geteuid()
                and left[item['path']]['uid'] != os.geteuid()):
            item['status'] = 'BLOCKED' if privileged else 'SKIP'
            item['detail'] = '当前进程无权恢复原属主'
    return comparisons + skipped


def execute(cli, args, password):
    env = os.environ.copy()
    env['DBACKUP_TEST_PASSWORD'] = password
    def disable_core_dump():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    start = time.monotonic()
    proc = subprocess.run([str(cli), *args], env=env, capture_output=True, text=True,
                          errors='replace', preexec_fn=disable_core_dump)
    try:
        payload = json.loads(proc.stdout.strip())
    except (ValueError, TypeError):
        payload = {'ok': False, 'error': cli_failure(proc)}
    if proc.returncode and payload.get('ok'):
        payload = {'ok': False, 'error': f'CLI 退出码 {proc.returncode}'}
    return payload, time.monotonic() - start


def run(root, cli, options):
    demo.validate_layout(root)
    restore = root / 'restore'
    if restore.is_symlink() or not restore.is_dir():
        raise tool.SafetyError('恢复根目录必须是非链接目录')
    run_dir = Path(tempfile.mkdtemp(prefix='dbackup-verify-', dir=restore))
    privileged = os.geteuid() == 0
    password = TEST_PASSWORD if options.encrypt else ''
    print(f'测试配置：打包={FORMAT_LABELS[options.format]}，压缩={COMPRESSION_LABELS[options.compression]}，'
          f'加密={"启用" if options.encrypt else "关闭"}，'
          f'用户={os.geteuid()}（{"提权" if privileged else "普通"}）', flush=True)
    print(f'测试程序：{cli}\n样例目录：{root}', flush=True)
    combined = []
    for group in dict.fromkeys(options.group or GROUPS):
        started = time.monotonic()
        archive = run_dir / (group + '.abk')
        restored = run_dir / (group + '-restored')
        source = root / group
        rows = []
        create = ['archive', 'create', '--root', str(root), '--source', str(source),
                  '--output', str(archive), '--format', options.format, '--algorithm', options.compression]
        source_bytes = regular_file_bytes(source)
        backup, backup_seconds = execute(cli, create, password)
        restore_seconds = None
        backup_warning = backup.get('data', {}).get('warning', '')
        restore_warning = ''
        if not backup.get('ok'):
            rows.append(result('FAIL', '备份', group, backup.get('error', '未知错误')))
        else:
            extraction = ['archive', 'extract', '--root', str(root), '--input', str(archive), '--output', str(restored)]
            extracted, restore_seconds = execute(cli, extraction, password)
            restore_warning = extracted.get('data', {}).get('warning', '')
            if not extracted.get('ok'):
                rows.append(result('FAIL', '恢复', group, extracted.get('error', '未知错误')))
            else:
                try:
                    demo.validate_restore_target(root, str(restored))
                    rows.extend(compare(source, restored, privileged, backup_warning, restore_warning))
                except (OSError, ValueError, tool.SafetyError) as exc:
                    rows.append(result('FAIL', '比较', group, exc))
        skipped_paths = [row['path'] for row in rows if row['status'] in ('SKIP', 'BLOCKED')]
        for category, warning in (('备份警告', backup_warning), ('恢复警告', restore_warning)):
            for line in warning.replace('\\n', '\n').splitlines():
                if not line:
                    continue
                expected_warning = (any(path in line for path in skipped_paths)
                                    or '套接字节点不包含原服务或连接' in line)
                rows.append(result('WARN' if expected_warning else 'FAIL', category, group, line))
        elapsed = time.monotonic() - started
        combined.extend(rows)
        counts = {s: sum(r['status'] == s for r in rows) for s in ('PASS', 'FAIL', 'SKIP', 'BLOCKED')}
        outcome = ('FAIL' if counts['FAIL'] else 'BLOCKED' if counts['BLOCKED'] or (privileged and counts['SKIP'])
                   else 'PARTIAL' if counts['SKIP'] else 'PASS')
        evidence = [f'uid={os.geteuid()}, privileged={privileged}', f'format={options.format}, compression={options.compression}, encrypted={options.encrypt}',
                    f'backup_seconds={backup_seconds:.2f}',
                    f'restore_seconds={restore_seconds:.2f}' if restore_seconds is not None else 'restore_seconds=not_run',
                    f'source_regular_bytes={source_bytes}',
                    f'archive_bytes={archive.stat().st_size}' if archive.is_file() else 'archive_bytes=not_created',
                    f'elapsed={elapsed:.2f}s', f'archive={archive}', f'restored={restored}', f'outcome={outcome}']
        tool.write_report(run_dir / (group + '-report.md'), f'DBackup {group} 验收', 'bash test.sh', rows, evidence, outcome)
        size_detail = (f'备份={archive.stat().st_size}字节' if archive.is_file()
                       else '备份=未生成')
        restore_detail = f'{restore_seconds:.2f}秒' if restore_seconds is not None else '未执行'
        print(f'{GROUP_LABELS[group]}：{STATUS_LABELS[outcome]}，备份={backup_seconds:.2f}秒，恢复={restore_detail}，'
              f'总计={elapsed:.2f}秒，来源={source_bytes}字节，{size_detail}', flush=True)
        print(f'  比较项：通过 {counts["PASS"]}，失败 {counts["FAIL"]}，'
              f'跳过 {counts["SKIP"]}，受阻 {counts["BLOCKED"]}')
        if archive.is_file() and source_bytes:
            percent = archive.stat().st_size * 100 / source_bytes
            print(f'  压缩比：{percent:.2f}%')
        print_details(rows)
    overall = ('FAIL' if any(r['status'] == 'FAIL' for r in combined)
               else 'BLOCKED' if any(r['status'] == 'BLOCKED' or (privileged and r['status'] == 'SKIP') for r in combined)
               else 'PARTIAL' if any(r['status'] == 'SKIP' for r in combined) else 'PASS')
    tool.write_report(run_dir / 'summary.md', 'DBackup 演示数据验收汇总', 'bash test.sh', combined,
                      [f'uid={os.geteuid()}, privileged={privileged}', f'format={options.format}, compression={options.compression}, encrypted={options.encrypt}',
                       'CLI 核心验收；不代表 GUI 手工验收'], overall)
    print(f'总体结果：{STATUS_LABELS[overall]}\n报告目录：{run_dir}', flush=True)
    return exit_status(combined, privileged)


def main():
    options = parser().parse_args()
    root = demo.canonical_root(None)
    build = Path(os.environ.get('DBACKUP_BUILD_DIR', str(demo.PROJECT / 'out/build-test')))
    if not build.is_absolute():
        build = demo.PROJECT / build
    cli = build / 'BackupToolTestCli'
    if not cli.is_file() or not os.access(cli, os.X_OK):
        raise ValueError(f'找不到测试 CLI：{cli}；请先以普通用户运行 bash build.sh test')
    return run(root, cli, options)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, tool.SafetyError) as exc:
        print(f'验收失败：{exc}', file=sys.stderr)
        raise SystemExit(1)

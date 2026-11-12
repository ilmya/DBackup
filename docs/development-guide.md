# 开发与测试指南

## 模块与约定

- `core/filesystem/`：Linux 文件类型、元数据和安全路径操作；特殊文件的扫描和恢复还涉及备份模块。
- `core/selection/`：备份来源选择和筛选条件；`core/archive/`：扫描、打包、解包及恢复。
- `core/compression/`：压缩与解压；`core/security/`：加密、摘要和凭据存取。
- `core/incremental/`：文件级增量备份及版本链。
- `automation/schedule/`：Cron 定时规则；`automation/realtime/`：文件变化监控；`automation/` 中的计划管理器负责协调任务和执行。
- `gui/`：Qt Widgets 图形界面。
- `tests/`：Google Test、测试 CLI、验收脚本；`tools/acceptance/`：独立验收支持工具。

第一方 C++ 使用仓库的 `.clang-format`（四空格、100 列）；不格式化第三方代码和生成文件。Linux GCC、CMake、Qt 和依赖安装见 [README](../README.md)。应用密码通过 Secret Service 保存，不写入普通计划配置。

## 备份和安全

完整备份先扫描并校验来源，文件数据按 4 MiB 分块处理。自定义格式和 pax tar 均支持不压缩、Huffman、LZ77、DEFLATE 和可选 AES-256-GCM。Huffman、LZ77 自主实现，DEFLATE 使用 zlib；AES-256-GCM、SHA-256、PBKDF2 使用 OpenSSL。MD5 仅供摘要展示，不作为认证。

分块封装以 `DBPK` 和小端版本号 2 标识；可读取之前的 `DBPK0002` 分块封装，不能读取旧的 `DBPK0001` 整体内存封装。未压缩、未加密的纯 tar 与系统 `tar` 互通。加密块和结束记录使用独立 nonce 及认证数据；未加密摘要仅检测意外损坏。恢复时先认证并将打包数据暂存于受限权限的临时文件，完成校验后才发布恢复文件，因此需要额外磁盘空间。`ArchiveService::inspectMetadata` 只返回元数据；`inspect` 加载内容上限为 64 MiB。

恢复拒绝不安全路径及链接，通过同目录临时文件逐项提交；不可读来源、无法恢复的设备和元数据权限限制须报告。自动任务使用相同核心，部分完成或失败不触发保留清理。套接字只恢复节点，不恢复服务或连接。

## 自动测试与覆盖率

```bash
bash build.sh test
bash build.sh coverage
# 已构建时，使用同等密钥环隔离直接运行 CTest：
bash tests/run_in_test_keyring.sh ctest --test-dir out/build-test --output-on-failure
```

`test` 需要系统 Google Test（Ubuntu `libgtest-dev`）、`dbus-run-session` 和 `gnome-keyring-daemon`；测试在临时 HOME、D-Bus 和密钥环中运行，不访问个人密钥环或手工样例。`unit` 验证独立规则，`integration` 验证文件系统和模块协作，`support` 验证测试工具；用例和标签分别见 `tests/CMakeLists.txt`、`tests/CTestLabels.cmake`。测试不会自动调用 sudo，root 用户下依赖普通用户权限的案例可能跳过。

repository 增量仓库实现及其专用测试已移除。回归保留完整归档、文件级增量差异/清单/版本链、定时和实时计划；计划兼容测试验证旧仓库计划被跳过、新计划不再写入 `repository` 字段，以及完整和增量计划仍可加载。历史仓库计划只在读取兼容逻辑中识别。

需要逐项测试输出时，在构建完成后运行 `ctest --test-dir out/build-test --show-only=json-v1` 导出测试目录；完整运行结果见 `out/build-test/Testing/Temporary/LastTest.log`。本次变更的测试条目与结果见[测试报告](repository-removal-tests.md)。

`coverage` 使用独立 GCC Debug 构建，只统计单元测试触及的生产代码行和分支，不包含 GUI、测试或第三方代码；不设门槛，也不证明正确性。默认结果位于 `out/build-coverage/coverage/summary.md`。构建目录及并行数可通过 README 列出的环境变量调整；`DBACKUP_TEST_DATA_ROOT`、`DBACKUP_TEST_TEMP_ROOT`、`DBACKUP_TEST_TMP_ROOT`、`DBACKUP_TEST_RUNTIME_ROOT` 可调整测试隔离位置。不要指向个人备份或演示目录。项目位于 `/mnt` 时逐用例临时数据默认使用 WSL 原生 `/tmp`。

## 独立演示数据验收

WSL 手工样例独立位于 `/home/ilm/DBackup-Demo`，包含 `common/`、`large-data/`、`special-files/` 和 `restore/`；项目不生成或重置它们。常规组含名称、路径、大小和时间差异，部分媒体扩展名只是占位；大数据组为 512 MiB 重复数据和 512 MiB 伪随机数据；特殊文件组含链接、FIFO、路径型套接字、权限样例和设备节点。

先以普通用户运行 `bash build.sh test` 构建测试 CLI，再运行：

```bash
bash test.sh                                  # 自定义打包、DEFLATE、加密；三组
bash test.sh --format tar --compression none --no-encrypt  # 纯 tar
bash test.sh --group large-data               # 只测试大文件
sudo bash test.sh --group special-files       # 单独进行提权验收
```

`--format` 可选 `custom`、`tar`；`--compression` 可选 `none`、`huffman`、`lz77`、`deflate`；`--group` 可重复指定三组。默认加密使用固定演示密码 `123`；可用 `--no-encrypt` 关闭加密，`--encrypt` 可显式启用加密。密码不写入命令行或报告。每次在 `restore/` 新建独立目录，保留备份、恢复结果和逐项报告，不覆盖旧结果。

如果测试 CLI 构建在非默认目录，提权运行时需显式传入该目录，例如 `sudo env DBACKUP_BUILD_DIR=/实际构建目录 bash test.sh --group special-files`；不要在 sudo 下重新构建。

要清空演示数据的 `restore/`，在项目根目录运行 `bash clear.sh`。它会删除该目录内的所有备份、恢复文件和报告，但保留目录本身及三组来源样例；如目录中有普通用户无权删除的内容，可自行运行 `sudo bash clear.sh`。

输出包含各组备份、恢复和总耗时、来源普通文件逻辑大小、备份大小及比较状态。压缩比为备份大小除以来源普通文件逻辑大小；硬链接按路径计入，备份大小包含格式开销。普通和提权模式使用同一备份恢复逻辑：普通用户预期权限跳过显示“部分完成”、退出码 0；意外失败返回 1；提权运行有跳过或受阻返回 2。需提权的设备节点可能受 WSL 或文件系统限制；不要打开块设备占位节点。

终端逐项显示失败、跳过、受阻和警告的路径及原因；完整的通过项见每组报告。测试 CLI 位于所选构建目录，更新代码后应先运行 `bash build.sh test`，以免使用旧版可执行文件。

`test.sh` 验证 CLI 核心，不代替 GUI 验收。已有 GUI 恢复结果也可执行 `bash tests/demo.sh compare common /home/ilm/DBackup-Demo/restore/common`，将组名和目标路径替换为实际值。比较覆盖内容、类型、权限、属主、时间和链接；详细场景及状态定义见[验收用例](acceptance-test-cases.md)，实际结果和未完成验收见[验证记录](verification.md)。

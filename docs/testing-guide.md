# DBackup Linux 自动测试指南

测试构建需要系统安装 Google Test 开发包（Ubuntu：`sudo apt install libgtest-dev`）；CMake 通过 `find_package(GTest REQUIRED)` 查找。仅构建产品并配置 `-DBUILD_TESTS=OFF` 时无需 Google Test。

## 常用命令

从项目目录运行：

```bash
bash build.sh test        # 完整回归；每次修改代码后使用
bash build.sh coverage    # 在独立 GCC 构建目录运行单元测试并生成覆盖率摘要
```

`test` 运行全部 unit、integration 和 support 标签。测试在临时主目录、隔离 D-Bus 会话和临时密钥环中执行；密钥环运行目录默认放在系统 `/tmp`，并在测试结束时清理；可通过 `DBACKUP_TEST_RUNTIME_ROOT` 更改。项目位于 `/mnt` 等挂载上时，脚本将逐用例临时目录放入系统 `/tmp`。失败时 CTest 会显示失败输出，因权限或环境跳过的用例会标为 skipped 并在 GTest 输出中保留原因。

演示样例的实际备份恢复通过独立的 `bash test.sh` 按需运行，参数、权限模式和退出码见 [README](../README.md#wsl-手工验收数据)。它不属于 `build.sh test` 的产品验收流程；仅其隔离自测属于自动回归。

覆盖率使用单独的 Debug/GCC 构建目录，默认是 `out/build-coverage/`；摘要位于 `out/build-coverage/coverage/summary.md`，gcov 明细位于同目录下的 `gcov/`。它只统计单元测试运行到的生产代码行与分支，不设置通过阈值，不统计 GUI/测试代码/第三方代码，也不能证明功能正确。

可通过 `QT_ROOT`、`DBACKUP_BUILD_DIR`、`DBACKUP_COVERAGE_BUILD_DIR` 和 `DBACKUP_JOBS` 指定 Qt、普通构建目录、覆盖率构建目录和并行数。自动测试的默认数据、临时文件及配置都在相应构建目录中，可通过 `DBACKUP_TEST_DATA_ROOT`、`DBACKUP_TEST_TEMP_ROOT`、`DBACKUP_TEST_TMP_ROOT` 覆盖；不要将这些变量指向个人备份或 `/home/ilm/DBackup-Demo`。

## 测试分类

分类依据是用例实际依赖和隔离程度，不是测试文件名：

- `unit`：密码学和压缩算法向量、六类筛选谓词与优先级、来源路径层级规则、特殊类型/元数据字段映射、Cron 解析，以及纯相对路径输入校验。这些用例只检查内存数据，不依赖真实文件系统、桌面会话、D-Bus 或用户密钥环。
- `integration`：归档与恢复、筛选预览一致性、加密封装、tar 互通、快照仓库与保留行为、真实临时文件系统、权限及特殊文件、实时监控、计划校验/调度/运行/持久化、CLI，以及 Qt offscreen 控件和布局检查。仓库保留数量、JobManager 计划校验和排队决策另有纯逻辑单元测试；真实仓库清理、配置保存和执行器交互仍通过公开服务流程验证。

归档检查需要文件内容时，`ArchiveService::inspect` 最多加载 64 MiB；检查较大归档应使用 `inspectMetadata`。压缩或加密归档的检查与恢复会先写入受限权限的临时打包文件，需要预留相应磁盘空间。分块格式不读取旧版 DBackup 封装；无压缩、无加密的 tar 仍可与系统 tar 互通。
- `support`：自动测试夹具生成、手工恢复结果比较器自测、用例目录结构校验。它们检查测试工具本身，不算作产品功能通过。

当前用例清单见 `tests/CMakeLists.txt` 和 `tests/CTestLabels.cmake`。单元测试集中于纯逻辑；档案读写、仓库清理、任务持久化和调度等跨模块行为保留在集成层。测试源码按归档、流水线、扫描、Linux 文件系统、仓库和自动任务归组；共享临时目录与小型文本夹具由 `tests/test_support.h` 管理，避免各套件重复实现清理逻辑。独立的 CLI/Qt 测试显式标注，夹具生成器、比较器自测和场景目录校验标为 `support`。

## 隔离与文件系统要求

`build.sh test*` 将自动生成的验收夹具和工具自测数据放在构建输出中。若项目位于 WSL `/mnt` 的 v9fs/DrvFS 挂载上，脚本会将自动删除的逐用例临时文件放入 `/tmp/dbackup-build-test-*`，因为该挂载不能可靠提供 POSIX 权限、FIFO、Unix socket、纳秒时间戳和 inotify；在 Linux 原生文件系统中的构建则默认把临时数据放在构建目录。可用 `DBACKUP_TEST_TMP_ROOT` 显式指定另一临时目录。测试密钥环在一次性 D-Bus 会话和临时 HOME 下运行，不读取或修改个人密钥环，也不访问 WSL 手工验收目录。需要特权的文件系统测试以普通用户运行；测试本身不会调用 sudo。若当前用户是 root，依赖普通用户权限边界的用例会跳过并说明原因。

权限、属主、硬链接、FIFO、Unix 套接字和 inotify 行为应在 WSL 原生 Linux 文件系统中验证。项目放在 `/mnt/e` 等 Windows 挂载目录时，权限位、链接、设备节点和监控语义可能受 DrvFS 限制；自动测试的临时数据也跟随构建目录，因此若要对这些系统语义作结论，应将项目/构建副本放在 WSL 原生文件系统内。测试没有 sudo 提权模式。

## 自动测试与 GUI 验收的边界

自动化不会操作或点击桌面窗口。Qt offscreen 用例只验证稳定的控件逻辑和布局，不等同于实际窗口交互。WSLg 窗口显示、文件/目录选择器、提示信息、进度和日志可读性、窗口关闭行为，以及 GUI 实际备份恢复，仍由用户人工验收。

独立的手工案例目录和比较方法见 [README 的 WSL 手工验收数据](../README.md#wsl-手工验收数据)。手工案例不是 `bash build.sh test` 的输入，完整回归不会清理或读取该目录。当前测试结果和限制见[功能验证记录](verification.md)。

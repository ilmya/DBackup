# DBackup

DBackup 是 Linux 图形化备份工具，支持完整备份和文件级增量备份，以及程序运行期间的定时和实时任务。两种备份可选择自定义格式或 POSIX pax tar、压缩算法及可选的 AES-256-GCM。普通用户无法读取或恢复的项目会报告为部分完成。

演示操作和三组样例说明见[演示指南](docs/demo-guide.md)。

旧版 repository 增量仓库功能已移除；现有仓库数据不会被删除，但不再支持读取或恢复。旧仓库自动任务在加载时跳过，并在下次保存计划时移除。完整备份和文件级增量版本链继续支持。

## 环境与启动

需要 Linux、C++17、CMake 3.21+、Ninja、Python 3、Qt 6.5+ Widgets、OpenSSL、zlib 和 libsecret。密码保存需要可用的 Secret Service 密钥环；WSL 用户可通过 WSLg 显示窗口。Ubuntu 依赖示例：

```bash
sudo apt install build-essential cmake ninja-build pkg-config python3 libssl-dev zlib1g-dev libsecret-1-dev fonts-noto-cjk
# 运行测试还需要：
sudo apt install libgtest-dev dbus-x11 gnome-keyring
```

Qt 需另装 Linux GCC 版本。`QT_ROOT` 默认是 `~/Qt/6.8.3/gcc_64`，可指定其他 Qt 前缀。`libgtest-dev` 仅为测试依赖。

```bash
bash build.sh             # 构建并启动
bash build.sh root        # 普通用户构建，以 root 启动
bash build.sh build       # 仅构建
bash build.sh test        # 完整自动测试，包含测试 CLI
bash build.sh coverage    # 单元测试的 GCC/gcov 覆盖率
bash build.sh help
```

常用环境变量为 `QT_ROOT`、`DBACKUP_BUILD_DIR`、`DBACKUP_COVERAGE_BUILD_DIR` 和 `DBACKUP_JOBS`。Linux 权限、特殊文件和实时监控建议在 WSL 原生文件系统验证，不要依赖 `/mnt` 挂载的 Windows 文件系统语义。

需要访问普通用户无权读取或恢复的文件时，可在终端运行 `bash build.sh root` 并按提示输入 sudo 密码。脚本以当前用户构建，并将当前图形会话变量传给 root 程序。root 模式使用独立的 root 配置和密钥环，创建的文件也可能归 root 所有；普通文件备份建议使用默认模式。root 会话不一定有可用的 Secret Service，使用需要保存密码的计划前应先确认密钥环可用。定时和实时任务仅在对应模式的程序运行期间执行。

## 文档

- [用户指南](docs/user-guide.md)：备份、恢复、计划、特殊文件及故障排查。
- [开发与测试指南](docs/development-guide.md)：模块、格式、安全、构建、测试和演示验收。
- [独立验收用例](docs/acceptance-test-cases.md)：自动和人工场景及成功判据；部分案例仍需手工执行。
- [验证记录](docs/verification.md)：已执行结果、演示报告和待完成的人工验收。

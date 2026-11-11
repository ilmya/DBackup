# WSL 2 与 Linux 开发

Ubuntu 22.04 WSL 2 可运行 Linux 构建；WSLg 将 Qt 应用显示为 Windows 桌面上的独立窗口。程序访问 WSL 文件系统。

在 Ubuntu 安装 CMake、Ninja、GCC、Python、OpenSSL、zlib 和 libsecret 开发库；Qt 需要 6.5+ 的 Linux GCC Widgets 套件。Ubuntu 22.04 软件源中的 Qt 版本可能不足，应另行安装。开发机 Qt 路径为 `/home/ilm/Qt/6.8.3/gcc_64`。

中文界面需要系统提供中文字体，例如 `fonts-noto-cjk`。自动测试需要 `dbus-run-session` 和 `gnome-keyring-daemon`；`build.sh test` 在临时主目录、D-Bus 会话和密钥环中运行 CTest。

```bash
cd /mnt/e/Code/DSync
bash build.sh       # 构建并启动
bash build.sh test        # 完整回归
bash build.sh coverage    # GCC/gcov 覆盖率摘要
```

`DBACKUP_BUILD_DIR` 和 `DBACKUP_COVERAGE_BUILD_DIR` 分别更改普通与覆盖率构建路径。Linux 权限、链接和 inotify 行为应优先在 WSL 原生文件系统中验证；`/mnt/e` 是 Windows 文件系统挂载，语义可能不同。应用的密码持久化需要已解锁的 Secret Service 密钥环与 D-Bus 用户会话。自动测试使用临时 D-Bus 会话和密钥环；验收夹具放在构建目录，项目位于 `/mnt` 时逐用例临时数据自动转到系统 `/tmp`。测试分组和覆盖率的范围见[测试指南](testing-guide.md)。

第一方 C++ 使用仓库根目录的 `.clang-format`（四空格缩进、100 列宽）。安装系统 `clang-format` 后，可在项目内预览格式差异：

```bash
find core automation gui tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

格式化时排除 `third_party/`、`out/` 和生成文件；核心 API 与算法步骤在头文件或实现函数前附有中文用途说明。

恢复路径、安全元数据、归档完整性和清理策略必须通过实际往返与错误场景验证，不可仅凭编译成功认定产品可用。

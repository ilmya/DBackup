# DBackup Linux 开发指南

## 模块

- `core/`：归档、快照仓库、凭据存储、加密和 Linux 文件操作。
- `automation/`：Cron、任务管理与 inotify 目录监控。
- `network/`、`server/`：HTTPS 客户端、远程仓库与 SQLite 服务端。
- `gui/`：Qt Widgets 界面；`tests/` 和 `tools/acceptance/` 提供测试与独立验收。

## 构建

在 Ubuntu 安装 README 中的依赖及 Qt 6.5+ Linux GCC 套件，执行 `bash build.sh` 构建并启动，或使用 `bash build.sh build`、`bash build.sh test`。通过 `QT_ROOT` 指定 Qt 前缀。WSL 中用 WSLg 显示应用窗口；涉及权限、链接、监控的测试应使用 Linux 原生文件系统。

`build.sh test` 使用临时主目录、D-Bus 会话和密钥环运行 CTest；需要 `dbus-run-session` 和 `gnome-keyring-daemon`。手动执行 CTest 时可用 `bash tests/run_in_test_keyring.sh ctest --test-dir <构建目录> --output-on-failure` 保持相同隔离。

## 数据与安全

恢复必须拒绝越界路径、危险链接和对目标路径中符号链接的穿越；写入需检查失败并在目标目录原子提交。存储和清理数据块时必须保护共享引用。密码和令牌通过 Secret Service 保存，不写入普通任务配置。

## 验收

执行完整构建、CTest、独立验收工具和 localhost HTTPS 测试；使用小规模数据验证文件内容、权限、UID/GID、时间、符号链接、硬链接、加密、取消与错误处理。环境无法提供密钥环或 WSLg 时应如实记录，不能当作通过。

# DBackup Linux 开发指南

## 模块

- `archive/`：Linux 文件扫描、归档格式、压缩加密及安全恢复。
- `repository/`：内容寻址数据块、快照与保留清理。
- `core/`：公共操作上下文、OpenSSL 原语与 Linux 文件操作。
- `automation/`：Cron、任务管理与 inotify 目录监控。
- `security/`：Secret Service 凭据存储。
- `network/`、`server/`：HTTPS 客户端、远程仓库与 SQLite 服务端。
- `gui/`：Qt Widgets 界面；`tests/` 和 `tools/acceptance/` 提供测试与独立验收。

## 构建

在 Ubuntu 安装 README 中的依赖及 Qt 6.5+ Linux GCC 套件，执行 `bash build.sh build` 或 `bash build.sh test`。通过 `QT_ROOT` 指定 Qt 前缀。WSL 中用 WSLg 显示应用窗口；涉及权限、链接、监控的测试应使用 Linux 原生文件系统。

## 数据与安全

Linux 归档和仓库采用独立格式，不迁移 Windows 数据。恢复必须拒绝越界路径、危险链接和对目标路径中符号链接的穿越；写入需检查失败并在目标目录原子提交。存储和清理数据块时必须保护共享引用。密码和令牌通过 Secret Service 保存，不写入普通任务配置。

## 验收

执行完整构建、CTest、独立验收工具和 localhost HTTPS 测试；使用小规模数据验证文件内容、权限、UID/GID、时间、符号链接、硬链接、加密、取消与错误处理。环境无法提供密钥环或 WSLg 时应如实记录，不能当作通过。

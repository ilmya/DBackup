# DBackup Linux 开发指南

## 模块

- `core/`：归档、快照仓库、凭据存储、加密和 Linux 文件操作。
- `automation/`：Cron、任务管理与 inotify 目录监控。
- `gui/`：Qt Widgets 界面；`tests/` 和 `tools/acceptance/` 提供测试与独立验收。

## 构建

在 Ubuntu 安装 README 中的依赖及 Qt 6.5+ Linux GCC 套件，执行 `bash build.sh` 构建并启动，或使用 `bash build.sh build`、`bash build.sh test`。通过 `QT_ROOT` 指定 Qt 前缀。WSL 中用 WSLg 显示应用窗口；涉及权限、链接、监控的测试应使用 Linux 原生文件系统。

`build.sh test` 使用临时主目录、D-Bus 会话和密钥环运行 CTest；需要 `dbus-run-session` 和 `gnome-keyring-daemon`。手动执行 CTest 时可用 `bash tests/run_in_test_keyring.sh ctest --test-dir <构建目录> --output-on-failure` 保持相同隔离。

## 数据与安全

完整归档按 4 MiB 块压缩、可选加密并写入新版本封装，文件内容不再整体载入内存；自定义格式和 pax tar 共用安全恢复。封装使用 SHA-256 检测损坏，加密时使用独立 nonce 的 AES-256-GCM 认证各块和结束记录；未加密摘要不防止恶意篡改。恢复先将校验后的打包数据暂存于受限权限的临时文件，再发布恢复结果，因此需要额外磁盘空间。旧版 DBackup 封装不兼容；纯 tar 与系统 tar 互通。Huffman、LZ77 与 MD5 在项目内实现，DEFLATE 使用 zlib，AES-256-GCM、SHA-256 和 PBKDF2 使用 OpenSSL。MD5 仅用于展示摘要，不作为认证。

分块封装以四字节 `DBPK` 标识开头，后接四字节小端版本号（当前为 2）。此前生成的 `DBPK0002` 分块封装仍可读取；旧版整体内存封装 `DBPK0001` 不支持。

恢复必须拒绝越界路径、危险链接和对目标路径中符号链接的穿越；写入需检查失败并在目标目录原子提交。存储和清理数据块时必须保护共享引用。密码和令牌通过 Secret Service 保存，不写入普通任务配置。

备份计划由任务管理器持久化，计划执行器供手动、定时和实时入口共用。独立归档采用不覆盖的原子提交；保留清理只在成功且无跳过项时运行。版本仓库仅在完整快照成功后清理。计划输出、筛选和调度设置随计划保存，密码留在密钥环。

扫描与格式层记录 FIFO、套接字及设备节点类型；设备节点记录主、次设备号。`OperationContext::allowDeviceNodes` 控制设备恢复。目标文件系统无法保留属主、权限或修改时间时，恢复结果携带警告；设备节点恢复需要相应权限。

## 验收

执行完整构建、CTest 和独立验收工具；使用小规模数据验证文件内容、权限、UID/GID、时间、符号链接、硬链接、套接字、加密、取消与错误处理。环境无法提供密钥环或 WSLg 时应如实记录，不能当作通过。

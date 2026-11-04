# DBackup Linux 设计

## 模块与数据流

Qt 界面调用归档与仓库接口，自动任务通过同一执行流程运行；网络客户端和 HTTPS 服务器负责远程数据传输。`core/` 包含 Linux 文件元数据、路径处理、操作进度和 OpenSSL 加密原语。

归档扫描 Linux 文件系统并记录普通文件、目录、符号链接、硬链接、POSIX 权限、UID/GID 和修改时间；对内容执行 zlib 压缩、SHA-256 完整性校验和可选 AES-256-GCM 认证加密。恢复先验证格式和路径，再通过目录文件描述符处理路径并使用临时文件发布内容。

增量仓库按文件内容分块，以 SHA-256 标识数据块并加密存储；快照清单保存元数据和块引用。清理前验证保留清单及引用块，并通过仓库锁协调创建、恢复和清理。Linux 数据使用新格式标记，不读取 Windows 旧格式。

## 自动化和系统集成

Cron 任务在程序运行期间调度；inotify 递归监视目录并处理目录重建及事件溢出。密码和令牌使用 Secret Service，桌面自启动使用 XDG autostart。系统托盘与密钥环依赖当前桌面会话，WSLg 支持应用以独立窗口显示。

## 构建和验证

项目使用 C++17、CMake、Qt Widgets/Network/Sql/HttpServer/WebSockets、OpenSSL、zlib 和 libsecret。构建与测试入口见 [开发指南](development-guide.md)；用户操作见 [用户指南](user-guide.md)。恢复和清理结果必须通过数据往返及失败场景验证，权限或目标文件系统限制需要明确报告。

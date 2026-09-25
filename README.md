# DBackup

DBackup 是面向 Windows 10/11 的图形化备份工具，支持便携完整归档、版本化增量仓库、自动任务、实时监控和 TLS 远程存储。

## 主要功能

- 同时选择多个文件和文件夹。
- `.abk` 完整归档，zlib 压缩和 AES-256-GCM 认证加密。
- 路径、类型、名称、时间、尺寸和用户筛选。
- 4 MiB 内容寻址块、SHA-256 去重和历史快照。
- Windows 属性、时间、ACL、符号链接和 Junction 元数据。
- Cron 计划、目录变化监控、保留策略和系统托盘。
- HTTPS 服务器、账户隔离、块级秒传和续传协议。
- 独立验收数据、SHA-256/时间/属性/链接/ACL 比较和 Markdown 报告。

## 运行环境

- Windows 10 或 Windows 11。
- 从源码构建需要 CMake 3.21+、Qt 6.5+ Widgets/Network/Sql/HttpServer 和匹配的 MinGW。
- 独立验收工具需要 Git Bash 和 Python 3。
- 生成测试证书需要 OpenSSL。

## 构建

可在 CMD、PowerShell 或 Git Bash 中调用统一构建脚本：

```bash
cmd.exe /c build.bat
cmd.exe /c build.bat test
cmd.exe /c build.bat package
```

如果 Qt 不在常见安装位置，先将 `QT_ROOT` 设为 Qt kit 目录。

- 客户端：`build/BackupTool.exe`
- 服务器：`build/DBackupServer.exe`
- 便携包：`build/DBackup-*.zip`

## 快速使用

1. 运行 `BackupTool.exe`。
2. 进入“新建备份”，添加文件或文件夹。
3. 选择完整归档或增量仓库，设置保存位置和密码。
4. 执行扫描预览，确认筛选结果后开始备份。
5. 定期将备份恢复到空目录，并使用验收工具比较。

完整操作见 [用户指南](docs/user-guide.md)。

## 验收工具

在 Git Bash 中运行：

```bash
./tools/acceptance/acceptance.sh self-test
./tools/acceptance/acceptance.sh generate --root D:/DBackup-Acceptance
```

工具数据位于仓库之外，不会进入 Git。完整功能、服务器和发布包验收步骤见 [完整验收指南](docs/testing-guide.md)。

## 文档

- [用户指南](docs/user-guide.md)
- [服务器指南](docs/server-guide.md)
- [完整验收指南](docs/testing-guide.md)
- [验收用例表](docs/acceptance-test-cases.md)
- [故障排查](docs/troubleshooting.md)
- [发布说明](docs/release-notes.md)
- [需求说明](docs/requirements.md)
- [系统设计](docs/design.md)
- [测试报告](docs/test-report.md)

## 安全边界

DBackup Server 用于本机或受信局域网，不应直接暴露到公网。首次连接自签名证书时必须核对 SHA-256 指纹。活动命名管道和 Windows 设备对象不会被伪装成普通文件备份。

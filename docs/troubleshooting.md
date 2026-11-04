# 故障排查

- **找不到 Qt 模块**：确认 Qt 是 Linux GCC 套件，且安装了 HTTP Server 和 WebSockets；将 `QT_ROOT` 设为 Qt 的 `gcc_64` 目录后重新配置。
- **窗口无法显示**：在 WSL 中检查 WSLg 是否可用；应用是 Linux 程序，不需要启动完整桌面。
- **中文显示为方框**：在 Ubuntu 安装支持中文的字体（例如 `sudo apt install fonts-noto-cjk`），重新启动应用。可用 `fc-list :lang=zh family` 检查是否存在中文字体。
- **无法保存密码或令牌**：确认 D-Bus 用户会话和 Secret Service 密钥环可用、已解锁；不要把密码写入任务配置。
- **权限或属主恢复失败**：确认目标文件系统支持 Linux 元数据，当前用户有相应权限。优先在 Linux 原生文件系统中测试。
- **归档或仓库格式无效**：检查选择的文件或目录是否正确，并确认数据完整。
- **实时监控无事件**：检查来源是否存在、是否具有读取权限，并优先在 Linux 原生文件系统验证；Windows 挂载盘的事件语义可能不同。

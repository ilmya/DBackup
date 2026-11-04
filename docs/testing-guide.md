# DBackup Linux 测试指南

## 构建与自动测试

```bash
QT_ROOT="$HOME/Qt/6.8.3/gcc_64" bash build.sh test
```

开发 CLI 和服务器随测试构建生成在 `out/build-test/`。可通过 `DBACKUP_BUILD_DIR` 更换构建目录。Linux 原生文件系统是权限、链接和 inotify 验证的主要环境。

## 独立验收

选择不会与真实数据冲突的专用目录；测试工具使用安全标记保护该目录：

```bash
bash tests/acceptance.sh self-test
bash tests/acceptance.sh generate --root "$HOME/DBackup-Acceptance" --profile standard
bash tests/acceptance.sh remote-e2e --root "$HOME/DBackup-Acceptance"
```

生成的目录包含测试文件、备份、服务器私钥和报告，不应提交。清理必须使用验收工具的安全确认；不要对真实备份做损坏或清理试验。

验证应覆盖普通文件、空目录、Unicode、权限、UID/GID、时间、符号链接、硬链接、多个来源、筛选、压缩和加密；另测错误密码、损坏数据、路径逃逸、取消、快照去重和保留策略。实时监控、计划任务、D-Bus 密钥环以及 WSLg 窗口需要在各自实际运行环境中验证。

自动测试覆盖底层功能、服务端和部分界面组件，不能代替通过界面实际选择文件、完成备份恢复、检查提示和关闭窗口的人工验收。当前结果和未验证范围见[功能验证](verification.md)。

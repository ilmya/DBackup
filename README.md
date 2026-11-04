# DBackup

DBackup 是 Linux 图形化备份工具，包含完整归档、版本化增量仓库、自动任务和 HTTPS 远程存储。Linux 版本不读取旧 Windows 版本的数据；需要旧数据时请先使用旧版本恢复。

## 环境

- Linux，C++17 编译器、CMake 3.21+、Ninja、Python 3。
- Qt 6.5+，包含 Widgets、Network、Sql、HttpServer 和 WebSockets。
- OpenSSL、zlib、libsecret 开发库；桌面会话需要可用的 Secret Service 密钥环。
- WSL 2 用户可通过 WSLg 在 Windows 桌面显示单独的应用窗口；程序访问的是 WSL 文件系统。

Ubuntu 依赖示例：

```bash
sudo apt install build-essential cmake ninja-build pkg-config python3 libssl-dev zlib1g-dev libsecret-1-dev
```

另行安装包含上述模块的 Linux Qt 6.5 或更新版本。当前开发机的 Qt 路径为 `~/Qt/6.8.3/gcc_64`。

## 构建和运行

```bash
QT_ROOT="$HOME/Qt/6.8.3/gcc_64" bash build.sh build
./out/build-test/BackupTool
bash build.sh test
bash build.sh install
```

可通过 `DBACKUP_BUILD_DIR` 和 `DBACKUP_INSTALL_PREFIX` 更改构建与安装路径。安装默认使用 `~/.local`。目录权限、硬链接和实时监控的验证推荐在 Linux 原生文件系统中进行，而不是 `/mnt` 下的 Windows 挂载盘。

详见 [用户指南](docs/user-guide.md)、[服务器指南](docs/server-guide.md)、[故障排查](docs/troubleshooting.md) 和 [开发说明](docs/wsl-linux-development.md)。

# DBackup

DBackup 是 Linux 图形化备份工具，包含完整归档、本地增量仓库和自动任务。

备份计划可保存来源、筛选和输出设置，手动或定时执行；版本仓库还支持实时监控。执行细节和跳过原因显示在活动日志中。

完整归档支持自定义格式或 POSIX pax tar，以及不压缩、Huffman、LZ77、DEFLATE 和可选 AES-256-GCM。纯 tar 可与系统 `tar` 互通；完成后显示的 MD5 仅用于摘要展示。增量仓库继续使用原有分块和 AES 加密。

## 环境

- Linux，C++17 编译器、CMake 3.21+、Ninja、Python 3。
- Qt 6.5+，包含 Widgets。
- OpenSSL、zlib、libsecret 开发库；桌面会话需要可用的 Secret Service 密钥环。
- WSL 2 用户可通过 WSLg 在 Windows 桌面显示单独的应用窗口；程序访问的是 WSL 文件系统。

Ubuntu 依赖示例：

```bash
sudo apt install build-essential cmake ninja-build pkg-config python3 libssl-dev zlib1g-dev libsecret-1-dev fonts-noto-cjk dbus-x11 gnome-keyring
```

另行安装包含上述模块的 Linux Qt 6.5 或更新版本。当前开发机的 Qt 路径为 `~/Qt/6.8.3/gcc_64`。

## 构建和运行

```bash
bash build.sh             # 增量构建并启动
bash build.sh build       # 仅构建
bash build.sh test
bash build.sh install
bash build.sh help
```

默认 Qt 路径为 `$HOME/Qt/6.8.3/gcc_64`；可通过 `QT_ROOT` 指定其他版本，通过 `DBACKUP_BUILD_DIR`、`DBACKUP_INSTALL_PREFIX` 和 `DBACKUP_JOBS` 更改构建目录、安装目录和并行数。安装默认使用 `~/.local`。目录权限、硬链接和实时监控的验证推荐在 Linux 原生文件系统中进行，而不是 `/mnt` 下的 Windows 挂载盘。

详见 [用户指南](docs/user-guide.md)、[故障排查](docs/troubleshooting.md) 和 [开发说明](docs/wsl-linux-development.md)。
# WSL 备份恢复展示

在项目目录运行 `bash tests/demo.sh`，脚本会在 `~/DBackup-Demo` 准备测试案例、构建程序、调用备份核心完成一次归档和恢复，并生成逐项比较报告。`test` 和 `restore` 是同级目录；每次恢复及归档都使用新的随机名称，不覆盖已有结果。使用 `bash tests/demo.sh prepare` 仅准备案例。

普通案例包含可压缩和随机数据、稀疏文件、可执行文件、带空格的名称和嵌套目录。稀疏文件检查内容和元数据，不检查恢复后是否仍以稀疏方式占用空间。

人工通过 GUI 将 `~/DBackup-Demo/test` 恢复到 `~/DBackup-Demo/restore` 下的新目录后，可运行 `bash tests/demo.sh compare ~/DBackup-Demo/restore/目录名` 检查结果。比较包含文件内容、文件类型、权限、属主、修改时间、链接目标及硬链接关系；报告位于 `~/DBackup-Demo/reports`。可通过 `DBACKUP_DEMO_ROOT` 指定专用演示目录。自动核心测试不代表 GUI 交互已经验收。

运行展示时还会在 `cases` 中分别生成套接字、无读取权限文件及字符/块设备案例，并单独输出 `*-cases.md` 报告。套接字检查节点往返；无读取权限文件检查是否报告原因；设备检查可选择跳过，并在具备权限时检查恢复。普通用户无法创建或恢复设备时报告 SKIP，不记为通过。案例中的设备节点权限为 `000`，测试只检查节点元数据，不打开设备。

需要完整检查受限文件、不同属主和设备节点时，在项目目录运行 `bash tests/demo.sh privileged`；脚本先以普通用户构建，再调用 sudo 执行独立案例，密码由用户自行输入。提权生成的案例、恢复文件及报告可能属于 root；即使使用 sudo，系统能力或文件系统不支持的项目仍会标记为 SKIP。也可用 `bash tests/demo.sh scenarios` 只运行普通权限特殊案例。

`bash build.sh test` 默认将自动测试的临时文件和验收数据放在同一演示目录的 `tmp` 和 `automation` 下，并使用隔离的密钥环；需要时可分别设置 `DBACKUP_TEST_DATA_ROOT` 和 `DBACKUP_TEST_TEMP_ROOT`。

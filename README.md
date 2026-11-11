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
sudo apt install build-essential cmake ninja-build pkg-config python3 libssl-dev zlib1g-dev libsecret-1-dev libgtest-dev fonts-noto-cjk dbus-x11 gnome-keyring
```

另行安装包含上述模块的 Linux Qt 6.5 或更新版本。当前开发机的 Qt 路径为 `~/Qt/6.8.3/gcc_64`。

自动化测试使用系统提供的 Google Test（Ubuntu 包 `libgtest-dev`）。仅构建产品时配置 `-DBUILD_TESTS=OFF` 无需该依赖。

## 构建和运行

```bash
bash build.sh             # 增量构建并启动
bash build.sh build       # 仅构建
bash build.sh test        # 单元、集成和支持工具的完整回归
bash build.sh coverage    # 独立 GCC/gcov 行与分支覆盖率报告
bash build.sh install
bash build.sh help
```

默认 Qt 路径为 `$HOME/Qt/6.8.3/gcc_64`；可通过 `QT_ROOT` 指定其他版本，通过 `DBACKUP_BUILD_DIR`、`DBACKUP_COVERAGE_BUILD_DIR`、`DBACKUP_INSTALL_PREFIX` 和 `DBACKUP_JOBS` 更改构建目录、覆盖率构建目录、安装目录和并行数。覆盖率报告写入独立构建目录，不设百分比门槛，也不代表功能正确性。安装默认使用 `~/.local`。目录权限、硬链接和实时监控的验证推荐在 Linux 原生文件系统中进行，而不是 `/mnt` 下的 Windows 挂载盘。

详见 [用户指南](docs/user-guide.md)、[故障排查](docs/troubleshooting.md) 和 [开发说明](docs/wsl-linux-development.md)。
# WSL 手工验收数据

演示数据固定放在 WSL 原生文件系统的 /home/ilm/DBackup-Demo，只包含 filters/、large-data/、special-files/ 和 restore/ 四个目录，以及安全标记。它与 bash build.sh test 使用的自动化测试数据相互隔离；自动化测试文件保存在构建目录中。

运行 `bash test.sh` 可使用默认的自定义归档、DEFLATE 和无加密配置，依次备份、恢复和比较三组数据。可用 `--format custom|tar`、`--compression none|huffman|lz77|deflate`、`--encrypt` 和 `--group filters|large-data|special-files` 指定配置。每次结果写入演示目录 `restore/` 下独立的新目录。测试使用 CLI 核心，并不代替 GUI 验收；归档按块处理，每个 CLI 进程默认限制为 3072 MiB，可用 `--memory-mib` 调整。

脚本输出各组备份、恢复和总耗时，以及来源普通文件逻辑大小和生成的归档大小；组报告还保留同样的数据和逐项比较结果。来源大小逐路径统计，硬链接按多个路径计数；它与归档大小的差额并非纯压缩收益，也包含归档元数据开销。

普通用户和提权模式使用相同的备份恢复流程；设备节点因权限不足而无法恢复时由程序跳过并报告。使用 `sudo bash test.sh` 独立进行提权验收。默认加密密码仅供测试，可通过 `DBACKUP_TEST_PASSWORD` 环境变量覆盖。普通模式的预期权限跳过会显示 PARTIAL 但返回 0；意外失败返回 1；提权模式有跳过时返回 2。先运行 `bash build.sh test` 构建测试 CLI。

样例独立保存于 WSL，不由项目生成或重置；项目保留只读比较来源和恢复结果的工具。

filters/ 按常见扩展名、路径、名称大小写、文件大小和固定修改时间组织样例；媒体和压缩格式文件是小型筛选占位文件，不保证能由相应软件打开。large-data/ 含 512 MiB 可压缩重复数据和 512 MiB 可复现伪随机数据，按块写入且不是稀疏文件。special-files/ 包含普通文件和目录、符号链接、硬链接、FIFO、路径型 Unix 套接字、不可读文件及可读邻居。套接字只保留节点，不包含服务或连接；抽象命名空间套接字没有文件路径，不在文件树样例范围内。

在 GUI 中分别选择一个来源目录进行备份，并恢复到 restore/ 下独立目录，例如 restore/filters。完成后可以逐项比较：

~~~bash
bash tests/demo.sh compare filters /home/ilm/DBackup-Demo/restore/filters
bash tests/demo.sh compare large-data /home/ilm/DBackup-Demo/restore/large-data
bash tests/demo.sh compare special-files /home/ilm/DBackup-Demo/restore/special-files
~~~

比较检查路径、文件内容、类型、权限、属主、修改时间、链接目标及硬链接关系。不可读来源文件会以 SKIP 列出，不会被误报为通过。报告写入 out/build-test/demo-reports/（或 DBACKUP_BUILD_DIR/demo-reports/），不会在演示根目录增加额外内容。large-data 比较需要读取约 1 GiB 来源和恢复数据，耗时会更长。

设备节点的创建和恢复需要相应权限；块设备样例只检查节点类型和设备号，不要打开该节点。所有特殊文件案例应在 WSL 原生 Linux 文件系统中验证。样例存在不代表 GUI 备份恢复已经通过。

bash build.sh test 使用独立 D-Bus 会话和临时密钥环；验收夹具和工具自测数据默认在构建目录中。项目位于 WSL `/mnt` 挂载时，逐用例临时文件会放入自动清理的 `/tmp` 目录，以获得 Linux 权限、链接和 socket 语义；可用 `DBACKUP_TEST_DATA_ROOT`、`DBACKUP_TEST_TEMP_ROOT` 和 `DBACKUP_TEST_TMP_ROOT` 覆盖测试位置。

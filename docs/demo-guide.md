# DBackup 演示指南

## 编译和启动

在 WSL 的项目根目录执行：

```bash
cd /mnt/e/Code/DSync
bash build.sh build   # 编译程序
bash build.sh         # 编译并启动图形界面
```

默认构建目录为 `out/build-test/`，图形程序为其中的 `BackupTool`。演示脚本使用单独的测试程序，先运行以下命令构建它并执行项目测试：

```bash
bash build.sh test
```

## 运行 test.sh

在项目根目录执行：

```bash
bash test.sh                                        # 三组样例：自定义格式、DEFLATE、加密
bash test.sh --group common                         # 只运行常规样例
bash test.sh --group large-data                     # 只运行大文件样例
bash test.sh --group special-files                  # 只运行特殊文件样例
bash test.sh --group common --group special-files   # 指定多组
bash test.sh --format tar --compression none --no-encrypt  # 不压缩、不加密的 tar
```

`--format` 可选 `custom`、`tar`；`--compression` 可选 `none`、`huffman`、`lz77`、`deflate`。加密默认开启，演示密码是 `123`；可用 `--no-encrypt` 关闭或 `--encrypt` 显式开启。需要对特殊文件执行提权演示时，可先以普通用户构建，再运行 `sudo bash test.sh --group special-files`。

脚本依次备份所选来源、恢复到新目录，再比较文件内容和元数据。每次结果保存在 `/home/ilm/DBackup-Demo/restore/dbackup-verify-*`，其中包含每组的备份文件、恢复目录、`*-report.md` 和汇总 `summary.md`。终端显示耗时、大小和通过、失败、跳过或受阻数量。普通用户遇到预期的权限跳过时可能显示“部分完成”且退出码为 0；失败返回 1，提权运行遇到跳过或受阻返回 2。脚本验证核心备份和恢复流程；图形界面操作需要另行演示。

脚本要求演示根目录顶层只包含标记文件 `.dbackup-acceptance.json`、三组来源目录和 `restore/`。当前演示目录还存在 `task/` 和 `task-backup/`，脚本会因此报告目录结构不符；演示前请自行将这两个目录移到 `DBackup-Demo` 之外并妥善保存。脚本不会生成或重置三组来源样例。

## 演示数据结构

三组数据位于 `/home/ilm/DBackup-Demo/`：

```text
DBackup-Demo/
├── .dbackup-acceptance.json  # 演示目录标记
├── common/                  # 常规文件与筛选样例
│   ├── documents/、images/、audio-video/、archives/、code/
│   ├── names/、sizes/、times/、users/
│   ├── cache/、logs/、复杂目录/、空目录/、隐藏和二进制/
├── large-data/              # 大文件和压缩效果样例
│   ├── repeat-01.bin … repeat-08.bin
│   └── random-01.bin … random-08.bin
├── special-files/           # 链接、权限和 Linux 特殊文件
│   ├── links/、permissions/、directories/
│   └── pipes/、sockets/、nodes/
└── restore/                 # 每次执行生成的备份、恢复结果和报告
```

### common：常规文件与筛选

- `documents/`、`images/`、`audio-video/`、`archives/`、`code/` 和 `logs/` 提供多种扩展名，可演示文件类型与名称筛选。部分图片、音视频和压缩包扩展名仅用于筛选演示，其内容是占位样例。
- `names/` 含 `REPORT.TXT`、`Report.txt`、`name with spaces.txt` 和 `中文文件名.txt`，可观察大小写、空格和中文路径。
- `sizes/` 含 0 字节、1 字节、1 KiB、1 MiB、5 MiB 和 10 MiB 文件；`times/` 包含不同修改时间的文件；`users/` 包含当前用户和 root 属主样例，可用于对应的筛选条件。
- `复杂目录/` 包含多层中文及空格路径，`空目录/` 用于验证空目录恢复；`隐藏和二进制/` 含隐藏文件、空文件和二进制字节样例。`cache/` 可用于演示排除临时文件。

例如在图形界面选择 `common/`，按扩展名筛选 `.txt`，先查看扫描预览，再备份和恢复，观察中文路径与空文件是否符合选择结果。

### large-data：大文件

`repeat-01.bin` 至 `repeat-08.bin` 是重复数据，`random-01.bin` 至 `random-08.bin` 是伪随机数据。每个文件 64 MiB，两类各 512 MiB，总计 1 GiB。可分别比较启用和关闭压缩时的备份大小、耗时及恢复比较结果；重复数据通常更容易压缩。

### special-files：链接、权限和特殊节点

- `links/relative-link` 是指向 `regular-target.txt` 的相对符号链接；`hardlink-a.bin` 与 `hardlink-b.bin` 是硬链接样例。
- `permissions/no-read.txt` 和 `readable-neighbor.txt` 用于观察不可读来源及其邻近文件；`directories/nested/` 和 `regular.txt` 提供普通目录和文件对照。
- `pipes/named-pipe` 是 FIFO，`sockets/endpoint.sock` 是路径型 Unix 套接字节点；恢复套接字节点不会恢复原有服务或连接。
- `nodes/char-null.dev` 和 `nodes/block-dummy.dev` 是设备节点样例。设备节点的创建和恢复受权限及文件系统支持影响，演示时不要打开块设备占位节点。

可先以普通用户执行特殊文件组，查看报告中的权限跳过，再按需要以 root 运行同一组并比较结果。

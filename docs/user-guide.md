# DBackup 用户指南

## 启动

在项目目录运行 `bash build.sh`，脚本会构建并启动程序。WSL 用户在 Ubuntu 终端运行该命令，WSLg 会将程序显示为独立窗口。在备份页的文件树中勾选文件或文件夹；目录包含后续新增的子项，取消勾选的子项不备份。树中可展开 Linux 主目录和其他挂载位置。

中文界面需要系统中文字体，例如 Ubuntu 的 `fonts-noto-cjk`；安装后重新启动应用。测试权限、链接和实时监控时应优先选择 WSL 原生文件系统，而非 `/mnt/e`。

## 完整归档与恢复

程序启动后直接进入“新建备份”，此页面用于创建完整归档。选择来源及保存位置，使用单选按钮选择打包格式，勾选“压缩”后选择压缩算法，勾选“加密”后输入 AES 密码。需要时设置筛选并查看预览，再开始备份。恢复时选择归档和目标目录，程序自动识别格式；加密归档需要输入相同密码。密码无法找回，请自行妥善保存。

打包可选择 DBackup 自定义格式或 POSIX pax tar，压缩可选择不压缩、Huffman、LZ77 和 DEFLATE。完整归档按块处理，不再受旧版 2 GiB 封装限制；备份和恢复需要额外空间存放临时打包数据，空间不足时操作会失败。新封装不兼容旧版 DBackup 封装。仅选择 tar 且不压缩、不加密时生成标准 `.tar`，可使用系统 `tar`；tar 加压缩或加密时使用 DBackup 恢复。加密归档会先完成认证，再恢复文件；未加密归档的摘要用于检测意外损坏，不能防止恶意篡改。备份完成显示的 MD5 只供摘要展示，不能用于认证文件；纯 tar 不提供内容完整性认证。选择单个目录进行新备份时，恢复目录下会保留该目录本身。

筛选可配置路径、类型、名称、修改日期、大小和所有者；文件类型页可选择普通文件、目录、符号链接、硬链接、FIFO、设备和套接字。日期按系统本地时间解释。扫描预览显示各类型的包含、排除数量和不可读文件提示。新建自动任务时会保存备份页当前的筛选规则。

FIFO、字符设备、块设备和路径型 Unix 套接字按类型及必要元数据记录，不读取其内容。恢复设备节点通常需要相应权限，目标文件系统必须支持；不需要设备节点时可取消勾选。套接字恢复的是文件节点，不包含原服务或连接，且可能占用服务路径。tar 使用 DBackup 的 PAX 扩展保存套接字；其他 tar 工具可能将其恢复为空文件。没有文件路径的抽象命名空间套接字不属于文件树备份范围。

读取权限不足的来源会跳过并报告，结果标记为部分完成；若所有内容均不可备份则报告失败。恢复时权限不足或文件系统不支持的项目也会报告。路径逃逸、危险链接和损坏数据仍会被拒绝。

## 增量仓库

在自动任务中新建计划并选择增量仓库目录和密码，通过立即运行或自动触发创建快照。快照页面可浏览和恢复版本；保留策略只应在确认保留版本可恢复后使用。备份与恢复均建议定期核对文件内容和元数据。

不要直接修改仓库配置、快照清单或数据块。权限、UID/GID 的恢复受当前账户权限和目标文件系统支持情况影响；操作失败时请查看应用提示。

## 备份计划

在“自动任务”页新建或编辑计划，选择来源、筛选规则、保存目录和备份类型。独立归档可手动或定时执行；版本仓库还可启用实时监控。可在列表立即运行、启用或停用计划，并查看最近结果。自动归档使用计划标识和随机文件名；保留数量仅作用于该计划生成的归档。部分完成或失败不会触发保留清理。

计划列表显示最近一次完整成功的时间；部分完成、失败或取消不会更新该时间。旧计划若最近结果不是完整成功，历史成功时间显示为暂无。

纯 tar 可使用系统 tar 查看；加密或压缩后的 DBackup 封装和版本仓库使用 DBackup 恢复。计划密码存放在桌面密钥环，保存或读取失败时需检查密钥环状态。活动日志记录执行阶段、路径与错误；关闭程序或停止 WSL 后，定时和实时任务暂停。

## 自动任务

可配置定时、实时或两者同时运行；定时任务使用 Cron 表达式，实时任务在应用运行期间监控选中来源。Cron 的问号提示说明五个字段、范围和示例；日期和星期都受限时满足任一条件即可；星期 0 或 7 表示周日。该语义与旧版不同，请检查原有计划。关闭窗口后程序可在托盘中继续运行；程序退出或 WSL 停止时任务暂停。任务密码通过 Secret Service 保存，需可用且已解锁的桌面密钥环。

实时任务的最近结果列出本次检测到的变化；为生成可恢复的完整快照，程序仍会扫描所选来源。若其他来源持续不可读，快照仍标记部分完成，完整原因可在活动日志及快照详情中查看，部分快照不会触发保留清理。


## 特殊文件提权演示

已有手工样例可通过 `bash test.sh` 验证三组备份恢复，或使用 `--group special-files --format tar --compression none` 等参数选择范围和算法；加密使用 `--encrypt`。普通用户模式记录权限跳过；在普通用户先运行 `bash build.sh test` 后，用户可自行执行 `sudo bash test.sh --group special-files` 进行提权验收。结果保存在演示目录 `restore/` 下每次新建的目录，不覆盖历史结果。该脚本验证 CLI 核心，不操作 GUI。

GUI 进程不能在运行中临时获得 root 权限；使用 root 启动 GUI 还会改变桌面会话和密钥环环境。现场展示设备节点时，可以在已有的隔离演示目录用 `sudo` 启动开发测试 CLI；它调用与 GUI 相同的归档核心，但不代表 GUI 交互已验收。首先在项目目录构建带测试 CLI 的程序：

```bash
bash build.sh test
```

确认已有设备样例类型和设备号正确后，用新的路径创建归档并恢复。以下路径假定 `bash build.sh test` 使用默认构建目录；如果设置过 `DBACKUP_BUILD_DIR`，请替换 CLI 路径。不要使用已有恢复目录或真实数据目录：

```bash
dbackup_demo_run=$(date +%Y%m%d-%H%M%S)
dbackup_demo_root=/home/ilm/DBackup-Demo
dbackup_demo_cli=/mnt/e/Code/DSync/out/build-test/BackupToolTestCli
sudo "$dbackup_demo_cli" archive create --root "$dbackup_demo_root" \
  --source "$dbackup_demo_root/special-files" \
  --output "$dbackup_demo_root/restore/special-$dbackup_demo_run.abk"
sudo "$dbackup_demo_cli" archive extract --root "$dbackup_demo_root" \
  --input "$dbackup_demo_root/restore/special-$dbackup_demo_run.abk" \
  --output "$dbackup_demo_root/restore/special-$dbackup_demo_run"
sudo env DBACKUP_BUILD_DIR=/tmp/dbackup-demo-report python3 -B tests/demo.py compare \
  special-files "$dbackup_demo_root/restore/special-$dbackup_demo_run"
```

检查 CLI JSON 输出的 `ok` 和 `warning`，比较报告应为 FAIL 0、SKIP 0 才能称对应来源完整往返通过。字符/块设备仍取决于 WSL 和文件系统是否允许 `mknod`；若准备或恢复被拒绝，应记录 SKIP/失败，不能记为通过。可用 `stat` 比较恢复前后节点类型和设备主次号，**不要打开块设备占位节点**。此流程只对命令行驱动提权，CLI 不提供 GUI 操作验收；恢复出来的 root 属主文件也不要用于覆盖个人数据。

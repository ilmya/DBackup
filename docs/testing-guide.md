# DBackup 人工验收操作指南

本文只说明需要人工执行的操作。自动测试不能代替以下 GUI、权限、托盘、网络和发布包验收。

每完成一项，在 `D:/DBackup-Acceptance/evidence` 保存截图，在 `reports` 保存比较报告。出现与“通过标准”不一致的结果时停止该项并记录，不要在原始资料上继续破坏性测试。

## 1. 验收前准备

### 1.1 准备软件

需要：

- Windows 10/11；
- Git Bash；
- Python 3；
- Qt 6.5 以上及配套 MinGW；
- OpenSSL 3 运行库（打包和服务器测试需要）；
- 至少 5 GB 可用磁盘空间；
- 建议启用 Windows 开发者模式，以创建符号链接。

所有命令均从仓库根目录的 Git Bash 执行。

### 1.2 构建并检查自动测试

```bash
cmd.exe /c build.bat test
./tests/acceptance.sh self-test
cmd.exe /c build.bat package
mkdir -p out/package/acceptance
python -m zipfile -e out/package/DBackup-portable.zip out/package/acceptance
PORTABLE_DIR="$(find out/package/acceptance -mindepth 1 -maxdepth 1 -type d -print -quit)"
```

通过标准：

- 客户端、服务器和测试目标构建成功；
- CTest 没有失败；
- 验收工具返回码为 0；
- 符号链接因权限不足被跳过时记录 `SKIP`，不能记 `PASS`。
- 便携包已解压，`PORTABLE_DIR` 指向包含两个程序的目录。

证据：保存终端最后一页截图为 `evidence/01-build-tests.png`。

### 1.3 生成测试数据

```bash
./tests/acceptance.sh generate \
  --root D:/DBackup-Acceptance \
  --profile standard

./tests/acceptance.sh prepare-links \
  --root D:/DBackup-Acceptance

./tests/acceptance.sh prepare-permissions \
  --root D:/DBackup-Acceptance
```

若没有 D 盘，把本文所有 `D:/DBackup-Acceptance` 替换为一个专用空目录。禁止使用盘符根目录、用户主目录或代码仓库。

通过标准：测试根目录中存在 `fixtures`、`restore`、`archives`、`repositories`、`manifests`、`reports` 和 `evidence`。

测试密码统一使用：

```text
DBackup-Acceptance-Only!2026
```

`standard` 配置生成紧凑的常规验收数据；需要长时间进度、取消、磁盘压力时，再显式使用 `--profile stress`。所有源文件、归档、仓库和报告都应放在带验收标记的测试根目录内。

### 1.4 开发测试 CLI

`BackupToolTestCli.exe` 仅在开发测试构建中生成，不包含在用户便携包。它直接调用归档、增量仓库和远程仓库业务代码；所有文件输入/输出路径必须位于有效的验收根目录。运行帮助：

```bash
./tests/acceptance.sh test-cli --help
```

示例：

```bash
./tests/acceptance.sh test-cli archive preview \
  --root D:/DBackup-Acceptance \
  --source D:/DBackup-Acceptance/fixtures/normal

export DBACKUP_TEST_PASSWORD='DBackup-Acceptance-Only!2026'
./tests/acceptance.sh test-cli archive create \
  --root D:/DBackup-Acceptance \
  --source D:/DBackup-Acceptance/fixtures/normal \
  --output D:/DBackup-Acceptance/archives/cli.abk
./tests/acceptance.sh test-cli archive inspect \
  --root D:/DBackup-Acceptance \
  --input D:/DBackup-Acceptance/archives/cli.abk
./tests/acceptance.sh test-cli archive extract \
  --root D:/DBackup-Acceptance \
  --input D:/DBackup-Acceptance/archives/cli.abk \
  --output D:/DBackup-Acceptance/restore/cli
```

增量仓库可使用以下命令；从 `snapshot` JSON 结果复制 `data.id` 传给 `restore`：

```bash
./tests/acceptance.sh test-cli repository snapshot \
  --root D:/DBackup-Acceptance \
  --repository D:/DBackup-Acceptance/repositories/cli \
  --source D:/DBackup-Acceptance/fixtures/incremental
./tests/acceptance.sh test-cli repository list \
  --root D:/DBackup-Acceptance \
  --repository D:/DBackup-Acceptance/repositories/cli
./tests/acceptance.sh test-cli repository restore \
  --root D:/DBackup-Acceptance \
  --repository D:/DBackup-Acceptance/repositories/cli \
  --id SNAPSHOT_ID --output D:/DBackup-Acceptance/restore/cli-repository
```

便携归档对单个目录来源会把目录内容恢复到目标目录根部；如需对增量仓库恢复结果比较并保留所选目录名，比较命令使用 `--single-directory-layout preserve`。成功退出码为 0，业务失败为 1，参数/环境错误为 2，测试根目录安全校验拒绝为 4。每次都会在标准输出输出一行 JSON；进度事件写在标准错误。

自动全套入口是 `cmd.exe /c build.bat test`：它构建应用、服务端、测试 CLI，运行 C++ 测试、验收工具自测、数据生成、场景目录校验和本机 HTTPS 远程闭环。只要单独检查本机远程流程，也可以运行：

```bash
./tests/acceptance.sh remote-e2e --root D:/DBackup-Acceptance
```

需要单独重跑自动用例时，用用例表中的自动编号选择对应 CTest 组；`--all` 等同于重新构建并运行全套：

```bash
./tests/acceptance.sh run --case S1-PACK-005
./tests/acceptance.sh run --case S4-REMOTE-001
./tests/acceptance.sh run --all
```

只有 `acceptance-scenarios.json` 中登记为自动的用例可用 `--case`。人工或环境阻塞用例应按后续章节操作并如实填写状态。

它会创建临时 TLS 证书、隔离数据库和回环服务器，并通过生产 `RemoteClient`、`RemoteRepositoryService` 执行注册登录、上传取消后续传、用户隔离、下载取消后续传、恢复校验和删除。该结果不代表 GUI、双机网络或公网部署验收通过。

需要检查已手工启动的本机服务端时：

```bash
./tests/acceptance.sh server-smoke \
  --root D:/DBackup-Acceptance \
  --server https://127.0.0.1:8443
```

此开发联测仅允许 localhost/127.0.0.1/::1，创建一次性账号、上传并删除测试快照、注销令牌；服务端数据库中的测试用户记录不会通过协议删除。应连接本节专用的可丢弃 `server-data/manual/data`，最后由清理测试根目录一并移除。不要连接真实服务或使用真实凭据。

## 2. 完整归档和恢复

### 2.1 普通多来源备份

1. 启动 `"$PORTABLE_DIR/BackupTool.exe"`。
2. 打开“新建备份”。
3. 添加文件夹：
   - `D:/DBackup-Acceptance/fixtures/normal`
   - `D:/DBackup-Acceptance/fixtures/compression`
4. 确认来源列表中正好有两个来源。
5. 选择“便携完整归档”。
6. 保存为 `D:/DBackup-Acceptance/archives/basic.abk`。
7. 压缩等级设为 6，密码留空。
8. 点击“扫描预览”，记录文件数、目录数和总大小。
9. 点击“开始备份”，等待成功提示。
10. 打开“恢复数据”，选择刚生成的 `basic.abk`。
11. 恢复到空目录 `D:/DBackup-Acceptance/restore/basic`。

完成后执行：

```bash
./tests/acceptance.sh compare \
  --source D:/DBackup-Acceptance/fixtures/normal \
  --source D:/DBackup-Acceptance/fixtures/compression \
  --restored D:/DBackup-Acceptance/restore/basic \
  --report D:/DBackup-Acceptance/reports/basic.md
```

通过标准：

- GUI 显示备份和恢复成功；
- 报告汇总没有 `FAIL`；
- 路径、SHA-256、mtime、属性和空目录均通过；
- 中文、空格和 Emoji 文件名没有乱码。

注意：`--source` 必须与 GUI 实际添加的来源一致，不能用整个 `fixtures` 比较部分恢复结果。

### 2.2 多文件和边界尺寸

1. 新建备份，只添加 `fixtures/normal` 中的空文件、1 字节文件和多个普通文件。
2. 再添加 `fixtures/normal` 中的 4 MiB 前、等于、后以及多块文件。
3. 生成 `archives/multi-files.abk` 并恢复到 `restore/multi-files`。
4. 使用与实际来源相同的多个 `--source` 参数执行比较。

通过标准：所有文件均存在且 SHA-256 一致；空文件大小为 0；没有静默遗漏或同名覆盖。

### 2.3 无效归档

分别尝试恢复：

- 一个空文件；
- 一个普通文本文件；
- `basic.abk` 的截断副本；
- 修改过头部或中间字节的副本。

通过标准：全部明确失败；程序不崩溃；恢复目录中不出现被当作成功结果的文件。

## 3. 压缩和加密

### 3.1 压缩效果

1. 对 `fixtures/compression` 创建不压缩归档 `compression-none.abk`。
2. 对相同来源创建压缩等级 9 的 `compression-9.abk`。
3. 在资源管理器中记录两者大小。
4. 分别恢复并运行比较。

通过标准：

- 重复数据的压缩归档明显更小；
- 固定种子随机数据即使没有变小也能正确恢复；
- 两次恢复的 SHA-256 均一致。

### 3.2 加密、错误密码和篡改

1. 使用测试密码创建 `encrypted.abk`。
2. 使用正确密码恢复到 `restore/encrypted-good` 并比较。
3. 使用错误密码恢复到新的空目录 `restore/encrypted-wrong`。
4. 复制归档为 `encrypted-tampered.abk`，用十六进制编辑器修改中间一个字节。
5. 尝试恢复篡改副本。

通过标准：

- 正确密码恢复通过；
- 错误密码明确失败且不产生有效输出；
- 篡改副本报告认证或完整性错误；
- GUI、日志和任务配置中不出现明文密码。

## 4. 六类筛选

所有筛选均使用 `fixtures/filters`。每次操作都先点击“扫描预览”，截图记录预计集合，再执行备份和恢复。

### 4.1 路径

仅包含 `documents`，排除 `build` 和 `cache`。

通过标准：恢复结果只出现预期路径，预览数量和实际恢复集合一致。

### 4.2 类型

依次测试文档、图片、音频、视频、压缩包和代码预设。

通过标准：每种预设只包含对应样本；未知扩展名不会错误归入其他类型。

### 4.3 名称

测试前缀、后缀、扩展名、通配符以及大小写不同的名称。

通过标准：结果符合界面说明，且预览和恢复一致。

### 4.4 时间

依次选择今天、最近 7 天、最近 30 天和自定义起止日期。

通过标准：今天、3 天前、10 天前、40 天前和边界时刻样本归类正确。

### 4.5 尺寸

分别设置 1 KiB、1 MiB、5 MiB、10 MiB 的上下限，并切换 B/KB/MB/GB 单位。

通过标准：边界值包含正确；最小值大于最大值时 GUI 在备份前拒绝。

### 4.6 用户

选择当前 Windows 用户进行筛选。如有另一个测试账户，再测试另一所有者文件。

通过标准：当前用户文件匹配；没有第二账户时该项记录 `BLOCKED`，不要自动创建系统账户。

### 4.7 组合规则

同时设置路径、类型、名称、时间、尺寸和用户条件。

通过标准：跨类别按 AND 组合，恢复集合与扫描预览完全一致。

## 5. 权限、属性和链接

### 5.1 ACL、只读和隐藏属性

1. 备份 `fixtures/permissions`。
2. 恢复到 NTFS 空目录。
3. 执行独立比较。
4. 查看只读、隐藏属性和“安全”属性页。

通过标准：内容正确；属性、mtime 和可恢复 DACL 一致。无权恢复 owner/group 时必须给出提示，不能把内容恢复错误显示成成功。

### 5.2 符号链接和 Junction

1. 确认 Windows 开发者模式已开启。
2. 重新执行 `prepare-links`。
3. 备份 `fixtures/links` 并恢复到空目录。
4. 检查恢复项仍为链接，而不是复制的目标内容。
5. 检查危险链接哨兵文件没有被修改。

通过标准：链接类型和目标一致；扫描不进入链接循环；绝对或逃逸链接被拒绝。

### 5.3 活动命名管道

在第二个 Git Bash 窗口运行：

```bash
./tests/acceptance.sh hold-pipe \
  --root D:/DBackup-Acceptance
```

在 DBackup 中尝试添加或扫描该管道。

通过标准：程序明确拒绝或跳过并说明原因，不生成伪普通文件，不崩溃。

## 6. 本地增量仓库

### 6.1 首次快照和未变化快照

1. 在“新建备份”中选择 `fixtures/incremental`。
2. 选择“版本化增量仓库”。
3. 仓库设为 `repositories/local`，输入测试密码。
4. 创建第一次快照。
5. 记录仓库统计：

```bash
./tests/acceptance.sh inspect-repository \
  --repository D:/DBackup-Acceptance/repositories/local \
  --report D:/DBackup-Acceptance/reports/repository-baseline.md
```

6. 不修改来源，再创建一次快照并再次记录统计。

通过标准：两个快照都可见；第二次不重复保存相同数据块，物理增长接近零。

### 6.2 单块变化

下面命令中的 `--version` 表示测试数据变化阶段，不是软件格式版本：

```bash
./tests/acceptance.sh mutate \
  --root D:/DBackup-Acceptance \
  --version 2
```

1. 创建新快照。
2. 再次记录仓库统计。
3. 分别恢复变化前和变化后的快照。
4. 与工具保存的对应来源清单比较。

增量仓库会保留所选来源目录名。若使用比较器手工比较单个仓库来源，加入 `--single-directory-layout preserve`；便携归档的默认布局则是展平单个目录来源。

通过标准：12 MiB 文件只修改小区域时，只新增相关块；两个快照都能恢复到各自状态。

### 6.3 取消和损坏块

1. 使用 `--profile stress` 生成 512 MiB 文件，开始快照后中途点击“取消”。
2. 确认历史中没有不完整快照，原快照仍可恢复。
3. 复制整个仓库作为破坏测试副本。
4. 在副本中删除一个 `.blk`，尝试恢复。
5. 恢复副本后再修改一个 `.blk` 字节，重新尝试恢复。

通过标准：取消不发布快照；缺块和损坏块都明确失败；不要在唯一仓库上执行破坏测试。

## 7. 计划、实时和托盘

### 7.1 定时任务

1. 新建一个下一分钟会触发的任务。
2. 来源使用 `fixtures/realtime`，仓库使用专用目录，保留数量设为 3。
3. 观察“下次运行时间”。
4. 等待任务触发，记录最近结果。
5. 手工点击“立即运行”。
6. 停用任务并等待下一次计划时间。

通过标准：到期只运行一次；立即运行有效；停用后不再触发；下次时间和最近结果正确。

如果当前 GUI 没有编辑、启停或立即运行控件，应将对应项目记为 `FAIL`，不能因为底层代码存在而记为通过。

### 7.2 保留策略

连续产生 5 个成功快照，保留数量为 3。

通过标准：最终只保留最近 3 个成功快照且都可恢复。随后制造一次权限失败，确认失败任务不会删除最后的有效快照。

### 7.3 实时备份

1. 启用 `fixtures/realtime` 的实时任务。
2. 创建一个文件，等待一次备份。
3. 在 3 秒内连续保存同一文件 10 次。
4. 执行重命名和删除。
5. 持续每秒修改一次超过 30 秒。
6. 将监控目录临时改名，等待错误状态，再恢复原名。

通过标准：快速变化被合并，不产生 10 次快照；持续变化不会无限延迟；目录恢复后自动重连并完整重扫；仓库输出不会反过来无限触发。

### 7.4 托盘和登录启动

1. 关闭主窗口，确认托盘图标仍存在且进程未退出。
2. 在托盘中暂停，修改监控文件，确认不触发。
3. 恢复后确认任务继续。
4. 选择托盘“退出”，确认进程结束。
5. 启用登录启动，注销并重新登录 Windows。

通过标准：关闭窗口只隐藏；暂停/恢复有效；只有显式退出终止；登录后程序进入可用后台状态。

## 8. 本机服务器验收

### 8.1 生成证书和启动服务器

```bash
./tests/acceptance.sh server-cert \
  --output D:/DBackup-Acceptance/server-data/manual/cert

"$PORTABLE_DIR/DBackupServer.exe" \
  --data D:/DBackup-Acceptance/server-data/manual/data \
  --cert D:/DBackup-Acceptance/server-data/manual/cert/server-cert.pem \
  --key D:/DBackup-Acceptance/server-data/manual/cert/server-key.pem \
  --listen 127.0.0.1 \
  --port 8443
```

通过标准：终端显示监听 `https://127.0.0.1:8443`。缺少证书、私钥或使用无效 PEM 时服务器必须拒绝启动。

在第二个终端运行：

```bash
./tests/acceptance.sh server-smoke \
  --root D:/DBackup-Acceptance \
  --server https://127.0.0.1:8443
```

通过标准：命令退出码为 0，JSON 中 `ok` 为 `true`，并且显示上传续传、用户隔离、下载恢复和删除均成功。此命令使用临时测试账号并删除测试快照。

### 8.2 注册、登录和证书指纹

1. 在客户端“存储位置”填写 `https://127.0.0.1:8443`。
2. 注册 Alice 和 Bob 两个测试账号。
3. 使用错误密码登录一次，再正确登录。
4. 首次连接核对并保存证书 SHA-256 指纹。
5. 退出并重新连接，确认不重复询问同一证书。
6. 停止服务器，生成另一张证书后重新启动。

通过标准：错误密码被拒绝；正确登录成功；证书变化后客户端阻止连接并显示旧、新指纹，未经显式更新不能继续。

### 8.3 远程上传、恢复和隔离

1. Alice 登录，上传 `fixtures/network` 的远程快照。
2. 在远程历史中查看详情并恢复到 `restore/remote-alice`。
3. 使用独立比较器验证恢复结果。
4. Bob 登录，尝试列出、恢复或删除 Alice 的快照。
5. Alice 再次上传相同数据。

通过标准：Alice 可恢复；Bob 看不到也不能操作 Alice 数据；第二次相同上传复用已有块。

### 8.4 断点续传和删除引用

1. 上传较大远程快照，在传输中禁用网络适配器。
2. 恢复网络并重新执行。
3. 下载恢复时再次进行一次断网测试。
4. 创建两个共享数据块的快照，删除其中一个，再恢复另一个。

通过标准：只传输缺失块；不提交不完整快照；已校验下载块被复用；删除一个快照不破坏另一个快照。

## 9. 局域网双机验收

1. 服务器证书必须包含服务器 DNS 名或局域网 IP 的 SAN。
2. 服务器监听局域网地址，仅在受信网络开放测试端口。
3. 客户端电脑通过 HTTPS 连接并核对指纹。
4. 执行注册、上传、历史查询、下载恢复和比较。
5. 用第二账户重复用户隔离测试。

通过标准：跨机 TLS、上传、恢复、隔离全部通过。记录服务器 IP、端口、证书指纹、两台电脑系统信息及报告路径。

## 10. GUI、进度和取消

1. 在 Windows 缩放 100%、150%、200% 下浏览全部页面。
2. 调整窗口到最小可用尺寸。
3. 对 `stress-512m.bin` 或 `stress-1g.bin` 执行备份、恢复、上传和下载。
4. 分别在扫描、写归档、恢复、上传和下载期间点击取消。

通过标准：

- 没有关键文字截断、控件重叠或不可点击；
- GUI 不出现 Sprint、待开发、占位、测试数量或调试堆栈；
- 进度显示阶段、当前文件、文件/字节数量、速度和 ETA；
- 取消后界面恢复可操作，不发布半成品。

## 11. 健壮性人工测试

所有破坏测试都使用副本。先准备归档副本：

```bash
./tests/acceptance.sh fault-copy --root D:/DBackup-Acceptance \
  --kind archive-truncate \
  --input D:/DBackup-Acceptance/archives/basic.abk \
  --output D:/DBackup-Acceptance/archives/faults/truncated.abk
./tests/acceptance.sh fault-copy --root D:/DBackup-Acceptance \
  --kind archive-tag \
  --input D:/DBackup-Acceptance/archives/encrypted.abk \
  --output D:/DBackup-Acceptance/archives/faults/tag-changed.abk
```

`archive-empty`、`archive-header` 也可用于生成对应样本。逐个在 GUI 的恢复页或 `test-cli archive inspect/extract` 中打开副本。预期所有损坏副本都明确失败，不崩溃、不产生被当作成功的恢复内容；认证标签损坏应在校验阶段中止。

仓库损坏样本只能从仓库副本生成：

```bash
./tests/acceptance.sh fault-copy --root D:/DBackup-Acceptance \
  --kind repo-missing \
  --input D:/DBackup-Acceptance/repositories/local \
  --output D:/DBackup-Acceptance/repositories/faults/missing-block
./tests/acceptance.sh fault-copy --root D:/DBackup-Acceptance \
  --kind repo-corrupt \
  --input D:/DBackup-Acceptance/repositories/local \
  --output D:/DBackup-Acceptance/repositories/faults/corrupt-block
```

在 GUI 或 CLI 指向故障副本恢复。预期报告缺块、哈希或认证失败；`repositories/local` 原仓库应仍可正常恢复。

测试错误密码时，在新的终端运行以下命令后再尝试恢复加密归档；完成后关闭终端或清除该环境变量：

```bash
export DBACKUP_TEST_PASSWORD='Wrong-Acceptance-Password!'
./tests/acceptance.sh test-cli archive extract \
  --root D:/DBackup-Acceptance \
  --input D:/DBackup-Acceptance/archives/encrypted.abk \
  --output D:/DBackup-Acceptance/restore/wrong-password
unset DBACKUP_TEST_PASSWORD
```

测试来源文件占用：

```bash
./tests/acceptance.sh hold-file \
  --root D:/DBackup-Acceptance \
  --path D:/DBackup-Acceptance/fixtures/normal/hello.txt
```

保持该终端不按 Enter，在另一个终端启动 GUI 备份同一个文件；记录程序是否清楚报告共享/访问冲突。完成后回到持锁终端按 Enter 释放句柄。

目标只读/拒绝写入使用 `prepare-permissions` 生成的隔离目录，恢复完成后必须先执行 `restore-permissions` 再清理。不要对真实用户文件设置拒绝 ACL。

VHD 磁盘满测试仅在有管理员协助时进行：在“磁盘管理”创建一个专用、固定大小约 1–2 GB 的 VHD，初始化并格式化为 NTFS，给它分配盘符；只把测试仓库和生成副本放在该盘。用少量数据逐步填充至空间不足，验证错误提示和已有快照，再在磁盘管理中卸载 VHD、删除该 VHD 文件。禁止对系统盘、真实移动盘或含个人数据的卷执行此测试。若没有管理员权限，记为 `BLOCKED`。

第二 Windows 用户测试不要自动创建系统账号：由管理员在“设置 → 账户 → 其他用户”创建一次性本地标准用户，并把一个单独测试文件的所有权设为该用户；退出当前账户、以测试用户登录后运行筛选/备份。完成后恢复 ACL 并删除该测试账号。无法提供第二用户时记录 `BLOCKED`，不要用当前用户文件冒充。

两台电脑联测只在可信局域网进行：服务端先生成包含其 DNS 名或局域网 IP SAN 的测试证书，只绑定局域网接口；防火墙只临时允许测试端口和可信子网。客户端核对证书 SHA-256 指纹，执行注册、上传、历史、下载恢复和独立比较；再用另一测试用户确认数据隔离。测试后关闭端口规则、停止服务并备份或清除测试数据。切勿把测试服务直接暴露到公网。

另外执行以下人工项：

- 恢复目录只读或无写权限；
- 备份期间删除来源文件；
- 归档被截断或篡改；
- 仓库配置、清单或块损坏；
- 服务端强制结束后重启；
- 同一任务被连续点击多次。

通过标准：程序不崩溃、不显示错误成功、不覆盖已有有效备份；错误信息包含失败对象和原因；重新启动后仍可使用先前成功结果。

## 12. 便携发布包验收

```bash
cmd.exe /c build.bat package
```

1. 将 ZIP 复制到一台没有 Qt、CMake、MinGW 和源码的 Windows 10/11 电脑。
2. 解压到普通用户可写目录。
3. 直接启动 `BackupTool.exe`。
4. 完成一次普通归档、恢复和比较。
5. 检查托盘。
6. 启动 `DBackupServer.exe` 并完成本机 HTTPS 冒烟测试。

通过标准：不提示缺少 Qt、MinGW、SQLite、TLS 插件或 OpenSSL DLL；客户端和服务器均能完成基本操作。

## 13. 结果记录

每个用例至少保存：

```text
用例编号：
执行日期：
Git commit：
实际步骤：
实际结果：
状态：PASS / FAIL / SKIP / BLOCKED / MANUAL / WARN
截图或日志：
比较报告：
缺陷编号：
```

只有全部成功判据满足才能填 `PASS`。没有权限、第二台电脑或证书环境时填 `BLOCKED/SKIP`，不能直接略过。

详细编号和结果填写表见 [Sprint 1-5 验收用例](acceptance-test-cases.md)。

## 14. 测试数据清理

确认报告和截图已经另行保存后执行：

```bash
./tests/acceptance.sh restore-permissions \
  --root D:/DBackup-Acceptance

./tests/acceptance.sh cleanup \
  --root D:/DBackup-Acceptance \
  --yes
```

通过标准：只删除带验收工具安全标记的测试根目录；未标记目录、盘符根目录、用户目录和代码仓库必须被拒绝。

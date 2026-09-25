# DBackup 完整验收指南

本指南使用独立数据和比较器验证 DBackup，不使用被测程序的私有清单作为判定依据。

## 1. 环境准备

需要 Windows 10/11、Git Bash、Python 3、Qt 6.5 或更高版本。服务器测试还需要 OpenSSL。

在仓库根目录打开 Git Bash：

```bash
./tools/acceptance/acceptance.sh self-test
cmd.exe /c build.bat test
```

成功标准：工具自测返回码为 0，CMake 构建成功，CTest 无失败项。符号链接因 Windows 权限被跳过时，应记录为 `SKIP` 而不是 `PASS`。

## 2. 生成标准数据

```bash
./tools/acceptance/acceptance.sh generate --root D:/DBackup-Acceptance --profile standard
./tools/acceptance/acceptance.sh prepare-links --root D:/DBackup-Acceptance
./tools/acceptance/acceptance.sh prepare-permissions --root D:/DBackup-Acceptance
```

如果 D 盘不存在，将所有命令的 `D:/DBackup-Acceptance` 统一替换为专用空目录。不得使用盘符根目录、用户主目录或项目仓库。

生成后应存在 `fixtures`、`restore`、`archives`、`repositories`、`manifests`、`reports` 和 `evidence`。

## 3. 完整归档验收

1. 启动 `build/BackupTool.exe`。
2. 选择“新建备份”，添加 `fixtures/normal` 和 `fixtures/compression`。
3. 选择“便携完整归档”，保存为 `archives/basic.abk`。
4. 压缩等级设为 6，密码留空，开始备份。
5. 恢复到 `restore/basic`。
6. 执行：

```bash
./tools/acceptance/acceptance.sh compare \
  --source D:/DBackup-Acceptance/fixtures/normal \
  --source D:/DBackup-Acceptance/fixtures/compression \
  --restored D:/DBackup-Acceptance/restore/basic \
  --report D:/DBackup-Acceptance/reports/basic.md
```

`--source` 必须与 GUI 中实际添加的来源一致；选择多个来源时重复使用该参数。不要用整个 `fixtures` 与只恢复了部分子目录的结果比较。成功标准是报告中路径、SHA-256、时间、属性、空目录全部 `PASS`。

## 4. 压缩和加密

1. 分别对 `fixtures/compression` 创建压缩等级 0 和 9 的归档。
2. 对重复数据，等级 9 归档应明显小于等级 0；固定种子随机数据不一定变小。
3. 使用密码 `DBackup-Acceptance-Only!2026` 创建加密归档。
4. 正确密码恢复后比较应通过；错误密码应在写入有效文件前失败。
5. 复制归档并修改中间一个字节，恢复应报告认证或完整性错误。

## 5. 六类筛选

使用 `fixtures/filters` 逐项测试路径、类型、名称、时间、尺寸和用户。每设置一条规则，先记录“扫描预览”的包含/排除数量，再备份和恢复。恢复目录的实际路径集合必须与预览一致。

其他用户样本需要现有的 Windows 测试账户；没有时记录 `BLOCKED`，不要自动创建系统账户。

## 6. 权限、链接和特殊文件

1. 对 `fixtures/permissions` 备份并恢复，比较报告的 owner、group 和 DACL。
2. 对 `fixtures/links` 备份并恢复，链接类型和目标应一致，扫描不得跟随链接进入循环或外部路径。
3. 危险链接的哨兵文件不得被修改。
4. 在单独终端运行命名管道：

```bash
./tools/acceptance/acceptance.sh hold-pipe --root D:/DBackup-Acceptance
```

尝试将活动管道作为来源。程序应立即拒绝并保持可用，不得生成伪普通文件。

## 7. 增量、去重和取消

1. 将 `fixtures/incremental` 备份到 `repositories/local` 并保留首个快照。
2. 记录仓库基线：

```bash
./tools/acceptance/acceptance.sh inspect-repository \
  --repository D:/DBackup-Acceptance/repositories/local \
  --report D:/DBackup-Acceptance/reports/repository-v1.md
./tools/acceptance/acceptance.sh mutate --root D:/DBackup-Acceptance --version 2
```

3. 创建第二快照，再次检查仓库。一个 12 MiB 文件的小范围修改只应新增相关块，不应重复存储全文件。
4. 分别恢复两个快照，与对应版本清单比较。
5. 备份大文件时点击“取消”。不得出现可见的不完整快照，原有快照仍可恢复。
6. 复制仓库后删除或修改一个块，恢复应中止并指出块缺失或校验错误。

## 8. 计划、实时和托盘

1. 新建每分钟可触发的测试 Cron 任务，保留数设为 3。
2. 等待任务执行，确认“下次运行”和“最近结果”更新，且同一任务不会并发。
3. 连续修改 `fixtures/realtime` 内文件并执行重命名、删除。事件应在静默期后合并为一次备份。
4. 删除后重建被监控目录，确认状态可恢复或给出可理解错误。 
5. 关闭主窗口，任务应继续；托盘“暂停”后不应触发，“继续”后恢复。
6. 模拟失败后，确认最后一个成功快照未被保留策略删除。

## 9. 服务器验收

先按 [DBackup Server 使用指南](server-guide.md) 生成证书、启动服务器并运行 `server-smoke`。

然后在 GUI 中验证：

1. 两个账户都可注册和登录，错误密码被拒绝。
2. 首次连接显示指纹；重复连接不重复询问。
3. 替换证书后客户端必须阻止连接，不得静默信任。
4. Alice 创建的快照不能被 Bob 列出、下载或删除。
5. 上传两份相同数据，第二次应复用已有块。
6. 上传中断开网络，恢复后应从缺失块继续，不得提交不完整快照。
7. 远程恢复结果使用 `compare` 判定。
8. 删除一个快照后，其他快照引用的共享块必须仍可恢复。

跨机验收时，在服务器电脑记录启动命令、IP、端口和证书指纹，在客户端电脑保存连接、上传、恢复和比较报告。

## 10. 发布包验收

```bash
cmd.exe /c build.bat package
```

将生成的 ZIP 复制到没有 Qt 和开发工具的 Windows 10/11 电脑，解压后直接启动 `BackupTool.exe` 和 `DBackupServer.exe`。完成基础归档、恢复、托盘和 HTTPS 测试。

## 11. 证据与清理

将比较报告、服务器报告、必要截图和失败日志保存到 `reports` 或 `evidence`。测试完成后：

```bash
./tools/acceptance/acceptance.sh restore-permissions --root D:/DBackup-Acceptance
./tools/acceptance/acceptance.sh cleanup --root D:/DBackup-Acceptance --yes
```

`cleanup` 只会删除含有有效随机标记的验收根目录。未标记目录、盘符根目录、用户目录和仓库目录必须被拒绝。

更细的用例编号、结果填写栏和证据栏见 [Sprint 1–5 验收用例](acceptance-test-cases.md)。

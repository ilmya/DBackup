# DBackup 软件测试报告

## 1. 测试目标

验证 DBackup 本地归档、压缩加密、筛选、元数据、自动化、增量仓库和网络协议的正确性与失败安全性。

## 2. 测试环境

| 项目 | 配置 |
|---|---|
| 操作系统 | Windows 10 x64 |
| Qt | 6.8.3 MinGW kit |
| 构建系统 | CMake 4.3.0 |
| 编译器 | 匹配 Qt kit 的 MinGW GCC 13.1.0，C++17 |
| 测试框架 | 随仓库提供的最小 Google Test 1.14.0 |
| 压缩库 | Qt MinGW kit 内的 zlib 1.2.13 |
| 加密接口 | Windows CNG / bcrypt |
| 测试构建类型 | Debug；便携发布包为 Release |

## 3. 测试方法

执行命令：

```bash
cmd.exe /c build.bat test
./tests/acceptance.sh self-test
./tests/acceptance.sh validate-catalog
```

CTest 使用 `out/build-test/acceptance-e2e` 内的带标记隔离目录，并在完整测试流程中重建数据。该目录仅为自动测试使用；手工验收数据默认放在 `D:/DBackup-Acceptance`，两者不应混用。

## 4. 自动化测试用例

| 编号 | 测试用例 | 验证内容 |
|---|---|---|
| T01 | EmptyDirectory | 空目录打包和还原 |
| T02 | SingleFileRoundTrip | 单文件内容一致性 |
| T03 | MultipleFilesAndDirectories | 多文件和嵌套目录 |
| T04 | MtimeRestoration | 修改时间恢复 |
| T05 | PackNonexistentSource | 不存在源目录报错 |
| T06 | UnpackInvalidArchive | 非法 Magic 被拒绝 |
| T07 | UnpackNonexistentFile | 不存在归档报错 |
| T08 | ArchiveHeaderFormat | 默认打包入口生成统一归档头部 |
| T09 | LargeFileRoundTrip | 1 MiB 二进制文件往返 |
| T10 | EmptyFileRoundTrip | 空文件恢复 |
| T11 | CompressedEncryptedRoundTrip | 压缩、加密、正确/错误密码 |
| T12 | FilterByExtensionAndSize | 类型、扩展名和尺寸组合筛选 |
| T13 | StrictValidation | 筛选空格、数字、日期、类型和范围校验 |
| T14 | FileAttributesRestoration | Windows 只读属性恢复 |
| T15 | DetectsArchiveTampering | 当前归档篡改检测 |
| T16 | AtomicallyReplacesExistingArchive | 原子替换已有归档 |
| T17 | ExcludesDestinationArchiveFromSource | 目标归档位于源目录时不被重复打包 |
| T18 | SingleFileAsBackupSource | 单个文件作为完整备份来源并成功还原 |
| T19 | RejectsSourceEqualToDestination | 阻止源文件被输出归档覆盖 |
| T20 | MultipleBackupSources | 多个文件和文件夹组合打包与还原 |
| T21 | FilterAcrossAllSixCategories | 路径、类型、名称、时间、尺寸和用户组合筛选 |
| T22 | PreviewWithMultiExtensionAndWildcardRules | 多扩展名、通配符排除和扫描预览一致性 |
| T23 | CronExpressionTest | Cron 列表、范围、步长、非法输入和下次运行时间 |
| T24 | IncrementalDeduplication | 加密块仓库去重、恢复、错误密码和淘汰 |
| T25 | IncrementalSingleFileAndCancellation | 单文件边界和增量操作取消 |
| T26 | DirectoryWatcherTest | Windows 递归目录变化通知 |
| T27 | ArchiveAuthenticatedEncryption | AES-GCM、篡改检测和归档取消 |
| T28 | ArchiveSymbolicLinkAndAcl | 安全相对链接和安全描述符（需要系统权限） |
| T29 | UnicodeAndEmojiFileNamesRoundTrip | 中文和 Emoji 路径的宽字符读取、打包与恢复 |
| T30 | MultipleSourceDirectoryTimesRoundTrip | 多来源根目录和嵌套目录的修改时间恢复 |
| T31 | DirectoryMtimeRestoredAfterChildren | 文件创建完成后恢复目录修改时间 |
| T32 | RejectsInvalidArchiveFormat | 明确拒绝不符合归档布局的数据 |
| T33 | AcceptanceTestCliSmoke | 通过开发 CLI 执行归档/恢复和增量快照/恢复，并用独立清单比较 |
| T34 | DBackupRemoteClientServerE2E | 本机 TLS 服务端、真实远程客户端、用户隔离、上传/下载取消续传与恢复校验 |
| T35 | AcceptanceScenarioCatalog | 检查验收用例编号、自动用例映射、文档和数据声明一致 |

## 5. 测试结果

2026-10-14 回归结果：54 项 CTest 已注册，53 项通过、1 项因当前 Windows 环境未授予符号链接/安全描述符测试所需权限而跳过，0 项失败。包含核心 C++ 测试、验收工具自测、开发 CLI 实际归档/仓库往返、隔离本机 HTTPS 远程闭环和用例目录校验。

新增的核心加密测试通过 SHA-256 与 PBKDF2 已知向量、AES-GCM 往返、标签篡改拒绝和非法尺寸检查。

独立验收工具自测通过。自动测试生成的数据保留在 `out/build-test/acceptance-e2e`，完整 `build.bat test` 会通过标记保护流程重建；外部人工测试根目录必须使用 `tests/acceptance.sh cleanup --root ... --yes` 显式清理。

当前受限执行会话无法启动 Git Bash（系统返回共享信号管道访问拒绝），因此 Bash 包装脚本的真实交互尚需在普通 Git Bash 终端执行一次；底层 Python 工具、自测、数据生成和 CTest 均已在本机验证。

## 6. 需求覆盖情况

| 需求 | 覆盖状态 | 说明 |
|---|---|---|
| 普通文件和目录备份 | 已覆盖 | 空目录、单文件、嵌套目录 |
| 数据还原 | 已覆盖 | 内容、结构、mtime、只读属性 |
| 归档格式 | 已覆盖 | 所有打包入口使用统一布局，往返与无效格式拒绝测试通过 |
| 归档压缩 | 已覆盖 | 重复数据压缩标志及内容 |
| 归档加密 | 已覆盖 | AES-256-GCM 正确密码成功、错误密码和篡改失败 |
| 完整性检查 | 已覆盖 | 手工篡改单字节后拒绝读取 |
| 自定义筛选 | 部分覆盖 | 解析器和扩展名/尺寸/类型已覆盖；所有者依赖环境 |
| 原子归档替换 | 已覆盖 | 旧无效归档被完整新归档替换 |
| GUI 交互 | 人工验收 | 按 `docs/testing-guide.md` 记录截图和结果 |
| 开发 CLI 与自动数据比较 | 已覆盖 | 自动 CLI smoke 通过独立文件系统清单验证归档和仓库恢复 |
| 远程上传、隔离和续传 | 已覆盖（本机） | 临时 TLS 服务和真实 Qt 客户端；跨机和防火墙另行人工验收 |
| 超大文件性能 | 部分覆盖 | 标准自动数据控制规模；512 MiB/1 GiB stress 测试按指南手工执行 |

## 7. 建议的人工验收测试

1. 在另一台 Windows 10/11 电脑运行发布包，检查依赖 DLL。
2. 备份包含中文、空格和较深目录的真实资料。
3. 对同一目录分别执行无密码和有密码备份。
4. 使用错误密码还原，确认无文件输出。
5. 使用筛选表达式并人工核对归档内容。
6. 在目标目录已有同名文件时确认覆盖行为符合预期。
7. 测试磁盘空间不足、文件占用和权限不足场景。
8. 检查 GUI 在长时间操作期间仍可移动和刷新。

## 8. 已知风险

- 大文件和大量文件会占用较多内存。
- 当前 `.abk` 使用 AES-GCM；旧历史格式不属于本项目兼容目标，验收应使用当前程序重新生成样本。
- ACL 恢复依赖当前账户权限，符号链接测试在未授予创建链接权限的环境会跳过。
- 整个还原任务不是目录级事务，中途失败可能留下已完成文件。
- GUI 已提供备份进度和取消操作；筛选面板尚未提供可排序的任意规则列表。
- 尚未生成代码覆盖率和正式性能数据。

## 9. 测试结论

核心回归测试为 47 项通过、1 项权限相关跳过，0 项失败。Release 客户端、服务器和便携包均成功生成。便携 ZIP 为 15.82 MiB，解压后为 38.57 MiB；在不含 Qt/MinGW 的系统 PATH 下，服务器 `--help` 与客户端启动检查通过。跨机器发布、长期后台运行和实际断网续传仍需按验收指南人工记录。

## 10. 可移植构建验证

当前包已在本机解压目录执行干净 PATH 启动冒烟测试。仍需在未安装 Qt 和开发工具的另一台 Windows 10/11 电脑上执行最终验收；本机检查不能替代跨机器验证。

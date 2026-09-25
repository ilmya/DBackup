# DBackup 软件测试报告

## 1. 测试目标

验证 DBackup 本地归档、压缩加密、筛选、元数据、自动化、增量仓库和网络协议的正确性与失败安全性。

## 2. 测试环境

| 项目 | 配置 |
|---|---|
| 操作系统 | Windows |
| 构建系统 | CMake 4.3.0 |
| 编译器 | Qt MinGW GCC 13.1.0，C++17 |
| 测试框架 | Google Test 1.14.0 |
| 压缩库 | zlib |
| 加密接口 | Windows CNG / bcrypt |
| 构建类型 | Release |

## 3. 测试方法

执行命令：

```bash
cmd.exe /c build.bat test
./tools/acceptance/acceptance.sh self-test
```

测试使用自动创建的隔离目录，完成后删除测试数据。

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
| T08 | ArchiveHeaderFormat | v1 头部格式兼容 |
| T09 | LargeFileRoundTrip | 1 MiB 二进制文件往返 |
| T10 | EmptyFileRoundTrip | 空文件恢复 |
| T11 | CompressedEncryptedRoundTrip | 压缩、加密、正确/错误密码 |
| T12 | FilterByExtensionAndSize | 类型、扩展名和尺寸组合筛选 |
| T13 | StrictValidation | 筛选空格、数字、日期、类型和范围校验 |
| T14 | FileAttributesRestoration | Windows 只读属性恢复 |
| T15 | DetectsArchiveTampering | v2 归档篡改检测 |
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
| T27 | ArchiveV3AuthenticatedEncryption | v3 AES-GCM、篡改检测和归档取消 |
| T28 | ArchiveV3SymbolicLinkAndAcl | 安全相对链接和安全描述符（需要系统权限） |
| T29 | UnicodeAndEmojiFileNamesRoundTrip | 中文和 Emoji 路径的宽字符读取、打包与恢复 |
| T30 | MultipleSourceDirectoryTimesRoundTrip | 多来源根目录和嵌套目录的修改时间恢复 |

## 5. 测试结果

最新回归结果：31 项测试已注册，本机 30 项通过、1 项因 Windows 未授予符号链接创建权限而跳过，0 项失败。

独立验收工具自测覆盖确定性数据、哈希比较、mtime 容差、空目录、属性差异、链接不跟随和安全清理拒绝。

## 6. 需求覆盖情况

| 需求 | 覆盖状态 | 说明 |
|---|---|---|
| 普通文件和目录备份 | 已覆盖 | 空目录、单文件、嵌套目录 |
| 数据还原 | 已覆盖 | 内容、结构、mtime、只读属性 |
| v1 兼容 | 已覆盖 | 头部及原有往返测试 |
| v2 压缩 | 已覆盖 | 重复数据压缩标志及内容 |
| v2 加密 | 已覆盖 | 正确密码成功、错误密码失败 |
| 完整性检查 | 已覆盖 | 手工篡改单字节后拒绝读取 |
| 自定义筛选 | 部分覆盖 | 解析器和扩展名/尺寸/类型已覆盖；所有者依赖环境 |
| 原子归档替换 | 已覆盖 | 旧无效归档被完整新归档替换 |
| GUI 交互 | 人工验收 | 按 `docs/testing-guide.md` 记录截图和结果 |
| 超大文件性能 | 未覆盖 | 当前仍是内存式实现 |

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
- 新 v3 已使用 AES-GCM；AES-CBC 只保留旧 v2 读取兼容。
- ACL 恢复依赖当前账户权限，符号链接测试在未授予创建链接权限的环境会跳过。
- 整个还原任务不是目录级事务，中途失败可能留下已完成文件。
- GUI 已提供备份进度和取消操作；筛选面板尚未提供可排序的任意规则列表。
- 尚未生成代码覆盖率和正式性能数据。

## 9. 测试结论

核心回归测试为 30 项通过、1 项因系统未授予符号链接权限而跳过，0 项失败。Qt 客户端和服务器已完成 Release 编译。跨机器发布、长期后台运行和实际断网续传必须按验收指南人工记录。

## 10. 可移植构建验证

便携 ZIP 应在未安装 Qt 和开发工具的 Windows 10/11 电脑上执行最终验收，结果不得由开发机构建成功替代。

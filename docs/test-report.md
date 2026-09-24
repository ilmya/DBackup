# DBackup 软件测试报告

## 1. 测试目标

验证 Sprint 1、Sprint 2 的打包、还原、压缩、加密、筛选、格式校验和可靠性改进，确认当前版本具备课程阶段演示条件。

## 2. 测试环境

| 项目 | 配置 |
|---|---|
| 操作系统 | Windows |
| 构建系统 | CMake 4.3.0 |
| 编译器 | MinGW GCC 8.1.0，C++17 |
| 测试框架 | Google Test 1.14.0 |
| 压缩库 | zlib |
| 加密接口 | Windows CNG / bcrypt |
| 构建类型 | Debug |

## 3. 测试方法

执行命令：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
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

## 5. 测试结果

最新回归结果：20 项测试全部通过，0 项失败。

本次完善前共有 12 项测试；新增 8 项测试覆盖：

- 严格筛选参数验证；
- Windows 文件属性恢复；
- 未加密 v2 归档篡改检测；
- 已有归档的原子替换。
- 目标归档位于源目录时的自包含防护。
- 单个文件来源和源/目标同路径保护。
- 多个文件和文件夹组合归档。

在新增测试第一次执行时，`2026-02-30` 被日期库自动归一化为三月日期，导致非法日期测试失败。实现随后改为在转换前保存年月日并与转换结果比较，问题已修复。

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
| GUI 交互 | 人工测试待补 | 自动化测试未直接操作 Win32 控件 |
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
- AES-CBC 尚未升级为 AES-GCM。
- 所有者和 ACL 不会恢复。
- 整个还原任务不是目录级事务，中途失败可能留下已完成文件。
- GUI 没有进度、取消和结构化筛选面板。
- 尚未生成代码覆盖率和正式性能数据。

## 9. 测试结论

当前版本通过 Sprint 1、Sprint 2 的 20 项自动化回归测试，核心备份还原、多来源选择、压缩加密、筛选校验和基础可靠性达到课程阶段演示要求。正式提交前仍应完成跨机器发布验证和人工 GUI 验收，并将结果截图附入最终 PDF 测试报告。

## 10. 可移植构建验证

项目支持在独立构建目录中完成 Release 配置、编译、20 项测试和 CPack ZIP 打包。MinGW 发布构建采用静态编译器运行库；应在每次正式发布时使用 `objdump -p BackupTool.exe` 或等效工具确认只剩 Windows 系统 DLL，并在未安装开发工具的 Windows 电脑上执行最终冒烟测试。

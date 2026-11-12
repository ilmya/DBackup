// 提供备份创建、检查、预览与恢复的主要入口
// 集中定义备份选项、文件条目和公开服务接口，供图形界面、自动计划及测试工具调用。
// 串联来源扫描、格式编码、压缩加密和安全恢复，向调用方传递进度、取消与错误
// Copyright 2026 BackupTool Team. All rights reserved

#ifndef DBACKUP_ARCHIVE_ARCHIVE_H_
#define DBACKUP_ARCHIVE_ARCHIVE_H_

#include "core/operation.h"
#include <cstdint>
#include <string>
#include <vector>

// 限定筛选结果包含的文件或目录类型
enum class EntryTypeFilter : uint8_t {
    Any = 0,
    FilesOnly = 1,
    DirectoriesOnly = 2,
};

// 表示一条路径模式的包含或排除规则
struct PathFilterRule {
    bool include = false;
    std::string pattern;
};

// 表示来源树中指定路径的包含或排除状态
struct SourceSelectionRule {
    std::string path;
    bool include = false;
};

// 描述路径、类型、名称、用户、大小、时间以及来源树的筛选条件；空值表示不限制
struct FilterOptions {
    std::string pathContains;
    std::string nameContains;
    std::string extension;
    std::vector<std::string> extensions;
    std::string ownerContains;
    std::vector<PathFilterRule> pathRules;
    std::vector<SourceSelectionRule> sourceRules;
    EntryTypeFilter type = EntryTypeFilter::Any;
    uint32_t typeMask = 0xff;
    uint64_t minSize = 0;
    uint64_t maxSize = 0;
    int64_t afterMtimeMs = 0;
    int64_t beforeMtimeMs = 0;
};

// 保存来源扫描后的包含、排除和不可读取项统计
struct BackupPreview {
    uint64_t includedTypes[8]{};
    uint64_t excludedTypes[8]{};
    uint64_t unreadableFiles = 0;
    uint64_t includedFiles = 0;
    uint64_t includedDirectories = 0;
    uint64_t includedBytes = 0;
    uint64_t excludedFiles = 0;
    uint64_t excludedDirectories = 0;
    uint64_t excludedBytes = 0;
};

// 定义可选择的自定义格式和 tar 格式
enum class PackFormat : uint8_t { Custom = 0, Tar = 1 };
// 定义可选择的备份压缩算法
enum class Compression : uint8_t { None = 0, Huffman = 1, Lz77 = 2, Deflate = 3 };

// 汇总打包格式、压缩、密码和筛选条件
struct PackOptions {
    PackFormat format = PackFormat::Custom;
    Compression compression = Compression::Deflate;
    int compressionLevel = 6; // zlib 压缩级别，范围为 0 到 9
    std::string password;     // 空密码表示不加密
    FilterOptions filter;
};

// 描述备份条目的类型、相对路径、元数据、链接目标及内容
struct ArchiveEntry {
    uint8_t type; // 0 普通文件，1 目录，2 符号链接，3 硬链接，4 管道，5 字符设备，6 块设备，7 套接字
    uint8_t flags = 0;        // 第 0 位表示内容经过 zlib 压缩
    std::string relativePath; // UTF-8 相对路径
    uint32_t mode = 0;        // POSIX 文件模式
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t deviceMajor = 0;
    uint32_t deviceMinor = 0;
    int64_t mtimeMs = 0;    // 毫秒精度的修改时间
    std::string owner;      // 可用时记录 UTF-8 属主名称
    std::string linkTarget; // 符号链接或硬链接目标
    uint64_t originalSize = 0;
    std::string data; // 解码后的文件内容，目录为空
};

// 解析界面传入的筛选表达式；遇到未知条件或非法值时返回 false 并写明原因
bool ParseFilterOptions(const std::string& spec, FilterOptions& filter, std::string& error);
// 校验程序化构造的筛选条件，防止越界或相互冲突的范围进入扫描流程
bool ValidateFilterOptions(const FilterOptions& filter, std::string& error);

// 对外提供备份创建、检查、预览与恢复接口
class ArchiveService {
  public:
    // 使用默认选项备份单个文件或目录；失败时返回错误原因
    static bool create(const std::string& sourcePath, const std::string& destFile,
                       std::string& error);

    // 按指定筛选、压缩和可选密码加密创建备份
    static bool create(const std::string& sourcePath, const std::string& destFile,
                       const PackOptions& options, std::string& error);

    // 将多个来源合并成一个备份，来源读取问题写入错误/警告结果
    static bool create(const std::vector<std::string>& sourcePaths, const std::string& destFile,
                       const PackOptions& options, std::string& error);
    // 可响应取消、进度回调及设备节点选项地创建多来源备份
    static bool create(const std::vector<std::string>& sourcePaths, const std::string& destFile,
                       const PackOptions& options, std::string& error,
                       const OperationContext& context);

    // 只扫描来源并汇总筛选结果，不读取文件内容或生成备份
    static bool preview(const std::vector<std::string>& sourcePaths, const FilterOptions& filter,
                        BackupPreview& preview, std::string& error,
                        bool includeSingleDirectoryRoot = false);

    // 检查备份目录并读取条目元数据；加密备份需要提供密码
    static bool inspect(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                        std::string& error);
    // 使用密码检查备份；密码错误或数据损坏时返回 false
    static bool inspect(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                        std::string& error, const std::string& password);
    // 检查元数据而不加载文件内容；适合大备份
    static bool inspectMetadata(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                                std::string& error, const std::string& password = "");

    // 将未加密备份安全恢复到目标目录
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error);
    // 使用密码恢复备份；路径逃逸或备份损坏时拒绝提交
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error, const std::string& password);
    // 按操作上下文恢复，可取消任务并控制设备节点创建及覆盖行为
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error, const std::string& password,
                        const OperationContext& context);
};

#endif // DBACKUP_ARCHIVE_ARCHIVE_H_

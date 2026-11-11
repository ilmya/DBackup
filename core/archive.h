// Copyright 2026 BackupTool Team. All rights reserved.

#ifndef DBACKUP_ARCHIVE_ARCHIVE_H_
#define DBACKUP_ARCHIVE_ARCHIVE_H_

#include "core/operation.h"
#include <cstdint>
#include <string>
#include <vector>

enum class EntryTypeFilter : uint8_t {
    Any = 0,
    FilesOnly = 1,
    DirectoriesOnly = 2,
};

struct PathFilterRule {
    bool include = false;
    std::string pattern;
};

struct SourceSelectionRule {
    std::string path;
    bool include = false;
};

// 描述路径、类型、名称、用户、大小、时间以及来源树的筛选条件；空值表示不限制。
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

enum class PackFormat : uint8_t { Custom = 0, Tar = 1 };
enum class Compression : uint8_t { None = 0, Huffman = 1, Lz77 = 2, Deflate = 3 };

struct PackOptions {
    PackFormat format = PackFormat::Custom;
    Compression compression = Compression::Deflate;
    int compressionLevel = 6; // zlib level (0..9)
    std::string password;     // empty means no encryption
    FilterOptions filter;
};

struct ArchiveEntry {
    uint8_t type; // 0 file, 1 directory, 2 symlink, 3 hard link, 4 FIFO, 5 char, 6 block, 7 socket
    uint8_t flags = 0;        // bit 0 = zlib-compressed payload
    std::string relativePath; // UTF-8 relative path
    uint32_t mode = 0;        // POSIX file mode
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t deviceMajor = 0;
    uint32_t deviceMinor = 0;
    int64_t mtimeMs = 0;    // modification time in milliseconds
    std::string owner;      // UTF-8 owner name, if available
    std::string linkTarget; // symbolic-link or hard-link target
    uint64_t originalSize = 0;
    std::string data; // decoded file data; empty for directories
};

// 解析界面传入的筛选表达式；遇到未知条件或非法值时返回 false 并写明原因。
bool ParseFilterOptions(const std::string& spec, FilterOptions& filter, std::string& error);
// 校验程序化构造的筛选条件，防止越界或相互冲突的范围进入扫描流程。
bool ValidateFilterOptions(const FilterOptions& filter, std::string& error);

class ArchiveService {
  public:
    // 使用默认选项备份单个文件或目录；失败时返回错误原因。
    static bool create(const std::string& sourcePath, const std::string& destFile,
                       std::string& error);

    // 按指定筛选、压缩和可选密码加密创建归档。
    static bool create(const std::string& sourcePath, const std::string& destFile,
                       const PackOptions& options, std::string& error);

    // 将多个来源合并成一个归档，来源读取问题写入错误/警告结果。
    static bool create(const std::vector<std::string>& sourcePaths, const std::string& destFile,
                       const PackOptions& options, std::string& error);
    // 可响应取消、进度回调及设备节点选项地创建多来源归档。
    static bool create(const std::vector<std::string>& sourcePaths, const std::string& destFile,
                       const PackOptions& options, std::string& error,
                       const OperationContext& context);

    // 只扫描来源并汇总筛选结果，不读取文件内容或生成归档。
    static bool preview(const std::vector<std::string>& sourcePaths, const FilterOptions& filter,
                        BackupPreview& preview, std::string& error,
                        bool includeSingleDirectoryRoot = false);

    // 检查归档目录并读取条目元数据；加密归档需要提供密码。
    static bool inspect(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                        std::string& error);
    // 使用密码检查归档；密码错误或数据损坏时返回 false。
    static bool inspect(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                        std::string& error, const std::string& password);
    // 检查元数据而不加载文件内容；适合大归档。
    static bool inspectMetadata(const std::string& archiveFile, std::vector<ArchiveEntry>& entries,
                                std::string& error, const std::string& password = "");

    // 将未加密归档安全恢复到目标目录。
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error);
    // 使用密码恢复归档；路径逃逸或归档损坏时拒绝提交。
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error, const std::string& password);
    // 按操作上下文恢复，可取消任务并控制设备节点创建及覆盖行为。
    static bool extract(const std::string& archiveFile, const std::string& destDir,
                        std::string& error, const std::string& password,
                        const OperationContext& context);
};

#endif // DBACKUP_ARCHIVE_ARCHIVE_H_

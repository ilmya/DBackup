#pragma once

#include "core/archive.h"
#include "core/linux_file.h"
#include <filesystem>
#include <functional>
#include <vector>

namespace dbackup::archive_scan {

struct Entry {
    std::filesystem::path source;
    std::string relativePath;
    linux_file::Metadata metadata;
    std::string owner;
    std::string linkTarget;
    // 0 regular, 1 directory, 2 symbolic link, 3 hard link, 4 FIFO, 5 character, 6 block, 7 socket.
    uint8_t type = 0;
};

// 扫描来源、应用共享筛选规则，并生成供归档或快照复用的条目列表。
bool collect(const std::vector<std::string>& sources, const std::filesystem::path& excluded,
             const FilterOptions& filter, std::vector<Entry>& entries, BackupPreview* preview,
             const OperationContext& context, std::string& error,
             bool includeSingleDirectoryRoot = false);

} // namespace dbackup::archive_scan

#include <string>
#include <vector>

namespace dbackup::archive_format {

struct Entry {
    archive_scan::Entry file;
    std::vector<uint8_t> data;
    bool compressed = false;
};

// 编码当前自定义归档格式；提供密码时加密整个归档负载。
bool encode(const std::vector<Entry>& entries, int compressionLevel, const std::string& password,
            std::vector<uint8_t>& output, std::string& error);
// 校验并解码自定义归档；格式、密码或认证不正确时拒绝数据。
bool decode(const std::vector<uint8_t>& input, const std::string& password,
            std::vector<Entry>& entries, std::string& error);
// 根据用户选择的打包、压缩和加密组合生成当前归档字节流。
bool encodeCurrent(const std::vector<Entry>& entries, const PackOptions& options,
                   std::vector<uint8_t>& output, std::string& error);
// 识别当前封装并还原条目；校验失败时不向调用方提交部分结果。
bool decodeCurrent(const std::vector<uint8_t>& input, const std::string& password,
                   std::vector<Entry>& entries, std::string& error);

} // namespace dbackup::archive_format

namespace dbackup::archive_restore {

// 写入一个普通文件的内容；提供者不得关闭目标文件描述符。
using ContentWriter = std::function<bool(const archive_format::Entry&, int,
                                         const OperationContext&, std::string&)>;

// 将已解码条目恢复到目标目录，并执行路径、链接和权限安全检查。
bool restore(const std::vector<archive_format::Entry>& entries,
             const std::filesystem::path& destination, const OperationContext& context,
             std::string& error);
bool restore(const std::vector<archive_format::Entry>& entries,
             const std::filesystem::path& destination, const OperationContext& context,
             std::string& error, const ContentWriter& contentWriter);

} // namespace dbackup::archive_restore

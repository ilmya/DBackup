#include "core/archive.h"
#include "core/archive_internal.h"
#include "core/backup_policy.h"
#include "core/crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace dbackup::archive_scan {
namespace fs = std::filesystem;
namespace {
// 判断文件系统错误是否属于权限不足，可降级为部分完成。
bool permissionFailure(int value) {
    return value == EACCES || value == EPERM;
}

// 将已扫描条目计入预览中的类型、文件/目录和字节统计。
void addPreview(BackupPreview& result, const Entry& entry, bool included) {
    const unsigned category =
        dbackup::backup_policy::filterCategory(entry.type, entry.metadata.links);
    if (included)
        ++result.includedTypes[category];
    else
        ++result.excludedTypes[category];
    const uint64_t bytes = entry.type == 0 ? entry.metadata.size : 0;
    if (entry.type == 1) {
        if (included)
            ++result.includedDirectories;
        else
            ++result.excludedDirectories;
    } else if (included) {
        ++result.includedFiles;
        result.includedBytes += bytes;
    } else {
        ++result.excludedFiles;
        result.excludedBytes += bytes;
    }
}
} // namespace

// 扫描所选来源并生成带元数据的条目，同时累计筛选预览和可恢复警告。
bool collect(const std::vector<std::string>& sources, const fs::path& excluded,
             const FilterOptions& filter, std::vector<Entry>& entries, BackupPreview* preview,
             const OperationContext& context, std::string& error, bool includeSingleDirectoryRoot) {
    entries.clear();
    if (preview)
        *preview = {};
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    if (!ValidateFilterOptions(filter, error))
        return false;
    if (sources.empty()) {
        error = "请至少选择一个备份来源";
        return false;
    }
    std::error_code ec;
    const fs::path excludedAbsolute =
        excluded.empty() ? fs::path() : fs::absolute(excluded, ec).lexically_normal();
    if (ec) {
        error = ec.message();
        return false;
    }
    std::set<fs::path> uniqueSources;
    std::set<std::string> names;
    std::map<std::pair<uint64_t, uint64_t>, std::string> hardlinks;
    auto process = [&](const fs::path& path, const fs::path& relative,
                       const fs::path& filterRelative, bool forceDirectory) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        if (!excludedAbsolute.empty() &&
            fs::absolute(path, ec).lexically_normal() == excludedAbsolute)
            return true;
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!dbackup::backup_policy::sourcePathSelected(path, filter.sourceRules))
            return true;
        Entry entry;
        entry.source = path;
        entry.relativePath = relative.generic_string();
        if (!linux_file::readMetadata(path, entry.metadata, error)) {
            if (permissionFailure(errno)) {
                context.warn(error);
                error.clear();
                return true;
            }
            return false;
        }
        const auto fileType = dbackup::backup_policy::fileTypeFromMode(entry.metadata.mode);
        if (!fileType) {
            error = "不支持的文件类型: " + path.string();
            return false;
        }
        entry.type = *fileType;
        if (const passwd* owner = getpwuid(entry.metadata.uid))
            entry.owner = owner->pw_name;
        const bool selected =
            forceDirectory || dbackup::backup_policy::matchesFilter(entry, filter, filterRelative);
        if (preview && selected && entry.type == 0) {
            int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0 && permissionFailure(errno)) {
                ++preview->unreadableFiles;
                context.warn("无法读取文件: " + path.string() + ": " + std::strerror(errno));
            } else if (fd >= 0)
                close(fd);
        }
        if (preview)
            addPreview(*preview, entry, selected);
        if (!selected)
            return true;
        if (!linux_file::validRelativePath(relative) || !names.insert(entry.relativePath).second) {
            error = "备份来源包含无效或重复路径: " + entry.relativePath;
            return false;
        }
        if (entry.type == 2) {
            entry.linkTarget = fs::read_symlink(path, ec).generic_string();
            if (ec) {
                error = "无法读取符号链接: " + path.string() + ": " + ec.message();
                return false;
            }
        } else if (entry.type == 0 && entry.metadata.links > 1) {
            const auto key = std::make_pair(entry.metadata.device, entry.metadata.inode);
            auto [it, inserted] = hardlinks.emplace(key, entry.relativePath);
            if (!inserted) {
                entry.type = 3;
                entry.linkTarget = it->second;
            }
        }
        entries.push_back(std::move(entry));
        return true;
    };
    for (const auto& source : sources) {
        fs::path absolute = fs::absolute(source, ec).lexically_normal();
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!excludedAbsolute.empty() && absolute == excludedAbsolute) {
            error = "备份源不能同时作为输出文件: " + source;
            return false;
        }
        if (!uniqueSources.insert(absolute).second)
            continue;
        linux_file::Metadata metadata;
        if (!linux_file::readMetadata(absolute, metadata, error)) {
            if (permissionFailure(errno)) {
                context.warn(error);
                error.clear();
                continue;
            }
            return false;
        }
        const bool directory = S_ISDIR(metadata.mode);
        const bool rootIncluded = includeSingleDirectoryRoot || sources.size() > 1 || !directory;
        fs::path rootName = absolute.filename();
        if (rootIncluded && rootName.empty()) {
            error = "无法使用文件系统根目录作为多个来源之一";
            return false;
        }
        if (rootIncluded && !process(absolute, rootName, rootName, directory))
            return false;
        if (!directory)
            continue;
        std::function<bool(const fs::path&)> walk = [&](const fs::path& directoryPath) {
            std::error_code scanError;
            fs::directory_iterator it(directoryPath, fs::directory_options::none, scanError), end;
            if (scanError) {
                if (permissionFailure(scanError.value())) {
                    context.warn(directoryPath.string() + ": " + scanError.message());
                    return true;
                }
                error = "无法扫描目录: " + directoryPath.string() + ": " + scanError.message();
                return false;
            }
            while (it != end) {
                const fs::path child = it->path();
                fs::path filterRelative = child.lexically_relative(absolute);
                fs::path relative = rootIncluded ? rootName / filterRelative : filterRelative;
                if (sources.size() > 1)
                    filterRelative = rootName / filterRelative;
                if (!process(child, relative, filterRelative, false))
                    return false;
                const auto status = it->symlink_status(scanError);
                if (scanError) {
                    if (permissionFailure(scanError.value()))
                        context.warn(child.string() + ": " + scanError.message());
                    else {
                        error = child.string() + ": " + scanError.message();
                        return false;
                    }
                    scanError.clear();
                } else if (fs::is_directory(status) &&
                           dbackup::backup_policy::mayContainSelectedSource(child,
                                                                            filter.sourceRules) &&
                           !walk(child))
                    return false;
                it.increment(scanError);
                if (scanError) {
                    error = "扫描目录失败: " + directoryPath.string() + ": " + scanError.message();
                    return false;
                }
            }
            return true;
        };
        if (!walk(absolute))
            return false;
    }
    return true;
}
} // namespace dbackup::archive_scan


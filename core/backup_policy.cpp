#include "core/backup_policy.h"

#include "core/archive_internal.h"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <iterator>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;

// 统一相对路径与冗余分隔符，便于按路径组件比较选择规则。
fs::path normalized(const fs::path& path) {
    return path.lexically_normal();
}

// 按完整路径组件判断子路径关系，避免前缀相同的兄弟目录误匹配。
bool pathWithin(const fs::path& pathValue, const fs::path& parentValue) {
    const fs::path path = normalized(pathValue);
    const fs::path parent = normalized(parentValue);
    if (path == parent)
        return true;
    const fs::path relative = path.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != fs::path("..");
}

// 统计规范化路径的组件数量，用于选取最具体的规则。
size_t pathDepth(const fs::path& path) {
    return static_cast<size_t>(std::distance(path.begin(), path.end()));
}

// 将筛选字符串转成小写以执行不区分大小写的匹配。
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

namespace dbackup::backup_policy {

std::optional<uint8_t> fileTypeFromMode(uint32_t mode) {
    const mode_t kind = static_cast<mode_t>(mode);
    if (S_ISREG(kind))
        return uint8_t{0};
    if (S_ISDIR(kind))
        return uint8_t{1};
    if (S_ISLNK(kind))
        return uint8_t{2};
    if (S_ISFIFO(kind))
        return uint8_t{4};
    if (S_ISCHR(kind))
        return uint8_t{5};
    if (S_ISBLK(kind))
        return uint8_t{6};
    if (S_ISSOCK(kind))
        return uint8_t{7};
    return std::nullopt;
}

unsigned filterCategory(uint8_t type, uint64_t linkCount) {
    return type == 0 && linkCount > 1 ? 3u : static_cast<unsigned>(type);
}

bool matchesFilter(const archive_scan::Entry& entry, const FilterOptions& filter,
                   const fs::path& relativePath) {
    const bool directory = entry.type == 1;
    const unsigned category = filterCategory(entry.type, entry.metadata.links);
    if (category >= 8 || !(filter.typeMask & (1u << category)))
        return false;
    if (filter.type == EntryTypeFilter::FilesOnly && directory)
        return false;
    if (filter.type == EntryTypeFilter::DirectoriesOnly && !directory)
        return false;

    const std::string path = lower(relativePath.generic_string());
    const std::string name = lower(relativePath.filename().string());
    bool includes = false;
    bool included = false;
    for (const auto& rule : filter.pathRules) {
        includes |= rule.include;
        if (fnmatch(lower(rule.pattern).c_str(), path.c_str(), 0) == 0) {
            if (!rule.include)
                return false;
            included = true;
        }
    }
    if (includes && !included)
        return false;
    if (!filter.pathContains.empty() && path.find(lower(filter.pathContains)) == std::string::npos)
        return false;
    if (!filter.nameContains.empty() && name.find(lower(filter.nameContains)) == std::string::npos)
        return false;
    if (!filter.ownerContains.empty() &&
        lower(entry.owner).find(lower(filter.ownerContains)) == std::string::npos)
        return false;

    if (!directory) {
        std::vector<std::string> extensions = filter.extensions;
        if (extensions.empty() && !filter.extension.empty())
            extensions.push_back(filter.extension);
        if (!extensions.empty()) {
            bool found = false;
            for (auto extension : extensions) {
                extension = lower(extension);
                if (!extension.empty() && extension[0] != '.')
                    extension.insert(extension.begin(), '.');
                if (!extension.empty() && name.size() >= extension.size() &&
                    name.compare(name.size() - extension.size(), extension.size(), extension) ==
                        0) {
                    found = true;
                }
            }
            if (!found)
                return false;
        }
    }

    if (filter.minSize && entry.metadata.size < filter.minSize)
        return false;
    if (filter.maxSize && entry.metadata.size > filter.maxSize)
        return false;
    const int64_t mtime = entry.metadata.mtimeNs / 1000000;
    if (filter.afterMtimeMs && mtime < filter.afterMtimeMs)
        return false;
    if (filter.beforeMtimeMs && mtime > filter.beforeMtimeMs)
        return false;
    return true;
}

bool sourcePathSelected(const fs::path& pathValue, const std::vector<SourceSelectionRule>& rules,
                        bool includeSelf, bool defaultWhenNoRules) {
    if (rules.empty())
        return defaultWhenNoRules;
    const fs::path path = normalized(pathValue);
    size_t bestDepth = 0;
    bool selected = false;
    for (const auto& rule : rules) {
        const fs::path root = normalized(fs::path(rule.path));
        if (!includeSelf && path == root)
            continue;
        if (!pathWithin(path, root))
            continue;
        const size_t depth = pathDepth(root);
        if (depth >= bestDepth) {
            bestDepth = depth;
            selected = rule.include;
        }
    }
    return selected;
}

bool mayContainSelectedSource(const fs::path& pathValue,
                              const std::vector<SourceSelectionRule>& rules,
                              bool defaultWhenNoRules) {
    if (sourcePathSelected(pathValue, rules, true, defaultWhenNoRules))
        return true;
    const fs::path path = normalized(pathValue);
    for (const auto& rule : rules) {
        if (!rule.include)
            continue;
        const fs::path selected = normalized(fs::path(rule.path));
        if (selected != path && pathWithin(selected, path))
            return true;
    }
    return false;
}

bool hasMixedSourceSelection(const fs::path& pathValue,
                             const std::vector<SourceSelectionRule>& rules,
                             bool defaultWhenNoRules) {
    const fs::path path = normalized(pathValue);
    const bool selected = sourcePathSelected(path, rules, true, defaultWhenNoRules);
    for (const auto& rule : rules) {
        const fs::path descendant = normalized(fs::path(rule.path));
        if (descendant != path && pathWithin(descendant, path) && rule.include != selected)
            return true;
    }
    return false;
}

std::vector<SourceSelectionRule>
toggleSourceSelection(const fs::path& pathValue, bool include,
                      const std::vector<SourceSelectionRule>& rules, bool defaultWhenNoRules) {
    const fs::path path = normalized(pathValue);
    std::vector<SourceSelectionRule> result;
    result.reserve(rules.size() + 1);
    for (const auto& rule : rules) {
        if (!pathWithin(fs::path(rule.path), path))
            result.push_back(rule);
    }
    const bool inherited = sourcePathSelected(path, result, false, defaultWhenNoRules);
    if (inherited != include)
        result.push_back({path.generic_string(), include});
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) { return left.path < right.path; });
    return result;
}

std::vector<std::string> selectedSourceRoots(const std::vector<SourceSelectionRule>& rules) {
    std::vector<std::string> candidates;
    for (const auto& rule : rules) {
        if (rule.include)
            candidates.push_back(normalized(fs::path(rule.path)).generic_string());
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<std::string> roots;
    for (const auto& candidate : candidates) {
        bool covered = false;
        for (const auto& root : roots) {
            if (pathWithin(fs::path(candidate), fs::path(root))) {
                covered = true;
                break;
            }
        }
        if (!covered)
            roots.push_back(candidate);
    }
    return roots;
}

bool archiveMetadataMatchesType(uint8_t type, uint32_t mode, uint32_t deviceMajor,
                                uint32_t deviceMinor) {
    const mode_t kind = static_cast<mode_t>(mode);
    const bool matchingType = (type == 0 && S_ISREG(kind)) || (type == 1 && S_ISDIR(kind)) ||
                              (type == 2 && S_ISLNK(kind)) || (type == 3 && S_ISREG(kind)) ||
                              (type == 4 && S_ISFIFO(kind)) || (type == 5 && S_ISCHR(kind)) ||
                              (type == 6 && S_ISBLK(kind)) || (type == 7 && S_ISSOCK(kind));
    if (!matchingType)
        return false;
    return (type == 5 || type == 6) || (deviceMajor == 0 && deviceMinor == 0);
}

} // namespace dbackup::backup_policy

bool ValidateFilterOptions(const FilterOptions& filter, std::string& error) {
    if (static_cast<unsigned>(filter.type) >
        static_cast<unsigned>(EntryTypeFilter::DirectoriesOnly)) {
        error = "文件类型条件无效";
        return false;
    }
    if (filter.maxSize && filter.minSize > filter.maxSize) {
        error = "最小文件大小不能大于最大文件大小";
        return false;
    }
    if (filter.afterMtimeMs && filter.beforeMtimeMs && filter.afterMtimeMs > filter.beforeMtimeMs) {
        error = "开始日期不能晚于结束日期";
        return false;
    }
    for (const auto& rule : filter.pathRules) {
        if (rule.pattern.empty()) {
            error = "路径规则不能为空";
            return false;
        }
    }
    for (const auto& rule : filter.sourceRules) {
        const fs::path path(rule.path);
        if (!path.is_absolute() || path.lexically_normal() != path) {
            error = "来源选择路径无效";
            return false;
        }
    }
    error.clear();
    return true;
}

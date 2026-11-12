// 实现来源选择和文件筛选的共用规则
// 定义来源规则解析与条目匹配接口，确保扫描、预览和备份使用一致的选择条件。
// 根据路径、类型、名称、属主、大小和时间判断条目，并检查备份类型与元数据
#pragma once

#include "core/archive/archive.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace dbackup::archive_scan {
struct Entry;
}

namespace dbackup::backup_policy {

// 将 POSIX 文件模式映射为备份条目类型；未知模式返回空值
std::optional<uint8_t> fileTypeFromMode(uint32_t mode);
// 计算供界面统计使用的文件类型分类，区分硬链接项目
unsigned filterCategory(uint8_t type, uint64_t linkCount);
// 判断单个条目是否满足全部筛选与路径包含/排除规则
bool matchesFilter(const archive_scan::Entry& entry, const FilterOptions& filter,
                   const std::filesystem::path& relativePath);

// 根据最深层来源规则判断路径是否被选中
bool sourcePathSelected(const std::filesystem::path& path,
                        const std::vector<SourceSelectionRule>& rules, bool includeSelf = true,
                        bool defaultWhenNoRules = true);
// 判断目录中是否仍可能包含重新选中的后代来源
bool mayContainSelectedSource(const std::filesystem::path& path,
                              const std::vector<SourceSelectionRule>& rules,
                              bool defaultWhenNoRules = true);
// 判断当前目录的子项是否同时存在选中与排除状态
bool hasMixedSourceSelection(const std::filesystem::path& path,
                             const std::vector<SourceSelectionRule>& rules,
                             bool defaultWhenNoRules = false);
// 切换路径的包含状态，并删除已被新规则覆盖的冗余规则
std::vector<SourceSelectionRule>
toggleSourceSelection(const std::filesystem::path& path, bool include,
                      const std::vector<SourceSelectionRule>& rules,
                      bool defaultWhenNoRules = false);
// 从树选择规则中提取最少数量的根来源
std::vector<std::string> selectedSourceRoots(const std::vector<SourceSelectionRule>& rules);

// 校验备份条目类型与 POSIX 模式、设备主次号是否相互匹配
bool archiveMetadataMatchesType(uint8_t type, uint32_t mode, uint32_t deviceMajor,
                                uint32_t deviceMinor);

} // namespace dbackup::backup_policy

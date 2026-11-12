// 为增量备份扫描来源并计算文件摘要
// 声明面向增量备份的扫描入口，同时返回可打包条目和用于差异比较的文件状态。
// 在完整扫描中生成备份条目与文件状态，避免将读取失败误判为删除
#pragma once

#include "core/archive/archive.h"
#include "core/archive/archive_internal.h"
#include "core/incremental/incremental_diff.h"

namespace dbackup::incremental {

// 完整扫描来源并计算普通文件的 SHA-256 摘要；扫描不完整时返回错误，避免误判删除
bool scan(const std::vector<std::string>& sources, const FilterOptions& filter,
          const OperationContext& context, std::vector<FileState>& states, std::string& error);

// 在同一次扫描中生成备份条目和带摘要的文件状态；扫描或计算失败时不修改输出
bool scanForArchive(const std::vector<std::string>& sources, const FilterOptions& filter,
                    const OperationContext& context, std::vector<archive_scan::Entry>& entries,
                    std::vector<FileState>& states, std::string& error);

} // namespace dbackup::incremental

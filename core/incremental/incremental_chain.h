// 管理文件级增量备份的完整生命周期
// 声明版本创建、公开历史、完整校验、恢复和保留清理接口及其结果结构。
// 创建和查询版本，验证备份链，在暂存区恢复指定版本并安全清理旧版本
#pragma once

#include "core/archive/archive.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace dbackup::incremental {

// 记录刚创建版本的序号、类型和文件变化
struct CreatedVersion {
    uint64_t sequence = 0;
    bool full = false;
    std::vector<std::string> added, modified, deleted;
};

// 描述版本列表中的基本信息与可用的文件变化摘要
struct VersionInfo {
    uint64_t sequence = 0;
    bool full = false;
    int64_t createdAtMs = 0;
    size_t fileCount = 0;
    size_t deletedCount = 0;
    bool changesAvailable = false;
    std::vector<std::string> added, modified, deleted;
    std::string directoryName;
};

// 在计划专用目录创建版本；每第七个版本开始新的完整备份链
// 本操作不会清理已有历史，失败时通过 error 返回原因
bool createVersion(const std::string& directory, const std::vector<std::string>& sources,
                   const PackOptions& options, const OperationContext& context,
                   CreatedVersion& created, std::string& error);

// 加载并验证所有已提交版本及备份内容；只有整条链验证成功才更新输出
bool listVersions(const std::string& directory, const std::string& password,
                  const OperationContext& context, std::vector<VersionInfo>& versions,
                  std::string& error);

// 不打开加密清单，读取未认证的明文版本摘要供界面展示
bool listPublicVersions(const std::string& directory, const OperationContext& context,
                        std::vector<VersionInfo>& versions, std::string& error);

// 在私有暂存目录重建指定版本，再原子提交到新目标；不会覆盖已有目标
bool restoreVersion(const std::string& directory, uint64_t sequence, const std::string& password,
                    const std::string& destination, const OperationContext& context,
                    std::string& error);

// 必要时先将最早保留的增量版本重建为完整版本，再清理旧版本
// 清理中断后，仍可见的版本应保持可恢复
bool pruneVersions(const std::string& directory, const std::string& password, size_t keepLatest,
                   const OperationContext& context, size_t& removed, std::string& error,
                   const PackOptions* packOptions = nullptr);

} // namespace dbackup::incremental

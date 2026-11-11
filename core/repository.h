#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/archive.h"
#include "core/operation.h"

namespace dbackup::repository_policy {
// Snapshot IDs are already ordered newest first by the repository.
size_t retainedCount(size_t snapshotCount, size_t keep);
} // namespace dbackup::repository_policy

struct SnapshotInfo {
    std::string id;
    int64_t createdAtMs = 0;
    uint64_t fileCount = 0;
    uint64_t logicalBytes = 0;
    uint64_t storedBytes = 0;
    bool partial = false;
    std::vector<std::string> warnings;
};

struct SnapshotOptions {
    std::vector<std::string> sources;
    FilterOptions filter;
    std::string password;
    uint32_t chunkSize = 4u * 1024u * 1024u;
};

class LocalRepository final {
  public:
    // 指定本地版本仓库根目录；构造时不会删除或改写已有数据。
    explicit LocalRepository(std::string root);
    // 使用密码创建或解锁仓库；已有配置损坏或密码错误时失败。
    bool initialize(const std::string& password, std::string& error);
    // 按筛选结果创建加密快照，并报告增量、取消和部分完成状态。
    bool createSnapshot(const SnapshotOptions& options, SnapshotInfo& snapshot, std::string& error,
                        const OperationContext& context = {});
    // 列出按创建时间从新到旧排列的快照及其警告摘要。
    bool listSnapshots(std::vector<SnapshotInfo>& snapshots, std::string& error);
    // 恢复指定快照，校验密码、数据完整性和目标路径安全性。
    bool restoreSnapshot(const std::string& id, const std::string& destination,
                         const std::string& password, std::string& error,
                         const OperationContext& context = {});
    // 删除指定快照并回收不再被引用的数据块。
    bool deleteSnapshot(const std::string& id, std::string& error);
    // 仅保留最新指定数量的快照，并回收未被引用的数据块。
    bool prune(size_t keepLatest, std::string& error);

  private:
    std::string root_;
    std::string activePassword_;
    std::vector<uint8_t> salt_;
};

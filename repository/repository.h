#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/operation.h"
#include "packer.h"

struct SnapshotInfo {
    std::string id;
    int64_t createdAtMs = 0;
    uint64_t fileCount = 0;
    uint64_t logicalBytes = 0;
    uint64_t storedBytes = 0;
};

struct SnapshotOptions {
    std::vector<std::string> sources;
    FilterOptions filter;
    std::string password;
    uint32_t chunkSize = 4u * 1024u * 1024u;
};

class IBackupRepository {
 public:
    virtual ~IBackupRepository() = default;
    virtual bool initialize(const std::string &password, std::string &error) = 0;
    virtual bool createSnapshot(const SnapshotOptions &options, SnapshotInfo &snapshot,
                                std::string &error, const OperationContext &context = {}) = 0;
    virtual bool listSnapshots(std::vector<SnapshotInfo> &snapshots, std::string &error) = 0;
    virtual bool restoreSnapshot(const std::string &id, const std::string &destination,
                                 const std::string &password, std::string &error,
                                 const OperationContext &context = {}) = 0;
    virtual bool deleteSnapshot(const std::string &id, std::string &error) = 0;
    virtual bool prune(size_t keepLatest, std::string &error) = 0;
};

class LocalRepository final : public IBackupRepository {
 public:
    explicit LocalRepository(std::string root);
    bool initialize(const std::string &password, std::string &error) override;
    bool createSnapshot(const SnapshotOptions &options, SnapshotInfo &snapshot,
                        std::string &error, const OperationContext &context = {}) override;
    bool listSnapshots(std::vector<SnapshotInfo> &snapshots, std::string &error) override;
    bool restoreSnapshot(const std::string &id, const std::string &destination,
                         const std::string &password, std::string &error,
                         const OperationContext &context = {}) override;
    bool deleteSnapshot(const std::string &id, std::string &error) override;
    bool prune(size_t keepLatest, std::string &error) override;

 private:
    std::string root_;
    std::string activePassword_;
    std::vector<uint8_t> salt_;
};

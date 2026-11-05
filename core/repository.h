#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/operation.h"
#include "core/archive.h"

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
    explicit LocalRepository(std::string root);
    bool initialize(const std::string &password, std::string &error);
    bool createSnapshot(const SnapshotOptions &options, SnapshotInfo &snapshot,
                        std::string &error, const OperationContext &context = {});
    bool listSnapshots(std::vector<SnapshotInfo> &snapshots, std::string &error);
    bool restoreSnapshot(const std::string &id, const std::string &destination,
                         const std::string &password, std::string &error,
                         const OperationContext &context = {});
    bool deleteSnapshot(const std::string &id, std::string &error);
    bool prune(size_t keepLatest, std::string &error);

 private:
    std::string root_;
    std::string activePassword_;
    std::vector<uint8_t> salt_;
};

#pragma once

#include "core/archive.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace dbackup::incremental {

struct CreatedVersion {
    uint64_t sequence = 0;
    bool full = false;
    std::vector<std::string> added, modified, deleted;
};

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

// Creates a version in a dedicated directory. The caller must use a separate
// directory for each plan. Every seventh version starts a new full chain.
// Existing history is never pruned by this operation.
bool createVersion(const std::string& directory, const std::vector<std::string>& sources,
                   const PackOptions& options, const OperationContext& context,
                   CreatedVersion& created, std::string& error);

// Load and verify all committed versions. Results are published only after the
// entire chain, including archive contents, has passed verification.
bool listVersions(const std::string& directory, const std::string& password,
                  const OperationContext& context, std::vector<VersionInfo>& versions,
                  std::string& error);

// Lists unauthenticated, plaintext summaries without opening encrypted manifests.
bool listPublicVersions(const std::string& directory, const OperationContext& context,
                        std::vector<VersionInfo>& versions, std::string& error);

// Reconstruct one version in a private staging directory, then atomically
// publish it as a new destination. Existing destinations are never replaced.
bool restoreVersion(const std::string& directory, uint64_t sequence, const std::string& password,
                    const std::string& destination, const OperationContext& context,
                    std::string& error);

// Keep at most keepLatest versions by first rebasing the oldest retained delta.
// If interrupted, remaining visible versions remain recoverable.
bool pruneVersions(const std::string& directory, const std::string& password, size_t keepLatest,
                   const OperationContext& context, size_t& removed, std::string& error,
                   const PackOptions* packOptions = nullptr);

} // namespace dbackup::incremental

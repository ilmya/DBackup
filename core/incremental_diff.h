#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::incremental {

// State is supplied by the scanner after a successful, complete scan.
struct FileState {
    std::string path;
    uint8_t type = 0;
    uint32_t mode = 0, uid = 0, gid = 0;
    int64_t mtimeNs = 0;
    uint64_t size = 0;
    uint32_t deviceMajor = 0, deviceMinor = 0;
    std::string linkTarget;
    // A content digest can be supplied by a caller that hashes regular files.
    std::string contentDigest;
};

struct Changes {
    std::vector<std::string> changed;
    std::vector<std::string> deleted;
};

// Reject unsafe and duplicate paths; outputs are replaced only on success.
bool compare(const std::vector<FileState>& previous, const std::vector<FileState>& current,
             Changes& changes, std::string& error);

} // namespace dbackup::incremental

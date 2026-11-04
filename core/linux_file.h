#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dbackup::linux_file {

struct Metadata {
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    int64_t mtimeNs = 0;
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    uint64_t links = 0;
};

bool readMetadata(const std::filesystem::path &path, Metadata &metadata, std::string &error);
bool restoreMetadata(const std::filesystem::path &path, const Metadata &metadata,
                     bool symlink, std::string &error);
bool validRelativePath(const std::filesystem::path &path);
// Returns an open descriptor owned by the caller, or -1 on failure.
int openDirectory(const std::filesystem::path &path, bool create, std::string &error);
bool ensureDirectories(const std::filesystem::path &path, std::string &error);
bool atomicReplace(const std::filesystem::path &temporary, const std::filesystem::path &destination,
                   std::string &error);

}  // namespace dbackup::linux_file

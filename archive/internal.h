#pragma once



#include "archive/archive.h"
#include "core/linux_file.h"
#include <filesystem>
#include <vector>

namespace dbackup::archive_scan {

struct Entry {
    std::filesystem::path source;
    std::string relativePath;
    linux_file::Metadata metadata;
    std::string owner;
    std::string linkTarget;
    // 0 regular file, 1 directory, 2 symbolic link, 3 hard link.
    uint8_t type = 0;
};

bool collect(const std::vector<std::string> &sources, const std::filesystem::path &excluded,
             const FilterOptions &filter, std::vector<Entry> &entries,
             BackupPreview *preview, const OperationContext &context, std::string &error,
             bool includeSingleDirectoryRoot = false);

}  // namespace dbackup::archive_scan


#include <string>
#include <vector>

namespace dbackup::archive_format {

struct Entry {
    archive_scan::Entry file;
    std::vector<uint8_t> data;
    bool compressed = false;
};

// A Linux-only format. The complete body is encrypted when a password is supplied.
bool encode(const std::vector<Entry> &entries, int compressionLevel, const std::string &password,
            std::vector<uint8_t> &output, std::string &error);
bool decode(const std::vector<uint8_t> &input, const std::string &password,
            std::vector<Entry> &entries, std::string &error);

}  // namespace dbackup::archive_format



namespace dbackup::archive_restore {

bool restore(const std::vector<archive_format::Entry> &entries, const std::filesystem::path &destination,
             const OperationContext &context, std::string &error);

}  // namespace dbackup::archive_restore

// Copyright 2026 BackupTool Team. All rights reserved.

#ifndef PACKER_H_
#define PACKER_H_

#include <cstdint>
#include <string>
#include <vector>

enum class EntryTypeFilter : uint8_t {
    Any = 0,
    FilesOnly = 1,
    DirectoriesOnly = 2,
};

/** Optional criteria used while scanning a source directory. Empty/zero values mean "not set". */
struct FilterOptions {
    std::string pathContains;
    std::string nameContains;
    std::string extension;
    std::string ownerContains;
    EntryTypeFilter type = EntryTypeFilter::Any;
    uint64_t minSize = 0;
    uint64_t maxSize = 0;
    int64_t afterMtimeMs = 0;
    int64_t beforeMtimeMs = 0;
};

struct PackOptions {
    int compressionLevel = 6;  // zlib level (0..9)
    std::string password;       // empty means no encryption
    FilterOptions filter;
};

struct ArchiveEntry {
    uint8_t     type;           // 0 = file, 1 = directory
    uint8_t     flags = 0;      // v2: bit 0 = compressed payload
    std::string relativePath;   // UTF-8 relative path
    uint32_t    mode = 0;       // reserved for permissions
    int64_t     mtimeMs = 0;    // modification time in milliseconds
    std::string owner;          // UTF-8 owner name, if available
    uint64_t    originalSize = 0;
    std::string data;           // decoded file data; empty for directories
};

class Packer {
 public:
    /** Pack a directory using the legacy, uncompressed and unencrypted format. */
    static bool pack(const std::string &sourceDir, const std::string &destFile, std::string &error);

    /** Sprint 2: pack with compression, optional password encryption and filters. */
    static bool pack(const std::string &sourceDir, const std::string &destFile,
                     const PackOptions &options, std::string &error);

    /** Read a v1/v2 archive. v2 encrypted archives require a password. */
    static bool readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error);
    static bool readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error, const std::string &password);

    /** Unpack a v1/v2 archive. v2 encrypted archives require a password. */
    static bool unpack(const std::string &archiveFile, const std::string &destDir, std::string &error);
    static bool unpack(const std::string &archiveFile, const std::string &destDir,
                       std::string &error, const std::string &password);
};

#endif  // PACKER_H_

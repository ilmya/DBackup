// Copyright 2026 BackupTool Team. All rights reserved.

#ifndef PACKER_H_
#define PACKER_H_

#include <cstdint>
#include <string>
#include <vector>
#include "core/operation.h"

enum class EntryTypeFilter : uint8_t {
    Any = 0,
    FilesOnly = 1,
    DirectoriesOnly = 2,
};

struct PathFilterRule {
    bool include = false;
    std::string pattern;
};

/** Optional criteria used while scanning a source directory. Empty/zero values mean "not set". */
struct FilterOptions {
    std::string pathContains;
    std::string nameContains;
    std::string extension;
    std::vector<std::string> extensions;
    std::string ownerContains;
    std::vector<PathFilterRule> pathRules;
    EntryTypeFilter type = EntryTypeFilter::Any;
    uint64_t minSize = 0;
    uint64_t maxSize = 0;
    int64_t afterMtimeMs = 0;
    int64_t beforeMtimeMs = 0;
};

struct BackupPreview {
    uint64_t includedFiles = 0;
    uint64_t includedDirectories = 0;
    uint64_t includedBytes = 0;
    uint64_t excludedFiles = 0;
    uint64_t excludedDirectories = 0;
    uint64_t excludedBytes = 0;
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
    uint32_t    mode = 0;       // Windows file attributes (v2); legacy v1 may be 0
    int64_t     mtimeMs = 0;    // modification time in milliseconds
    std::string owner;          // UTF-8 owner name, if available
    std::string linkTarget;     // v3: relative symbolic-link/junction target
    std::vector<uint8_t> securityDescriptor;  // v3: self-relative owner/group/DACL
    uint64_t    originalSize = 0;
    std::string data;           // decoded file data; empty for directories
};

/** Parse the GUI filter expression. Returns false for unknown keys or invalid values. */
bool ParseFilterOptions(const std::string &spec, FilterOptions &filter, std::string &error);

class Packer {
 public:
    /** Pack one file or a directory using the legacy, uncompressed and unencrypted format. */
    static bool pack(const std::string &sourcePath, const std::string &destFile, std::string &error);

    /** Sprint 2: pack with compression, optional password encryption and filters. */
    static bool pack(const std::string &sourcePath, const std::string &destFile,
                     const PackOptions &options, std::string &error);

    /** Pack multiple files and/or directories into one v2 archive. */
    static bool pack(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                     const PackOptions &options, std::string &error);
    static bool pack(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                     const PackOptions &options, std::string &error, const OperationContext &context);

    /** Scan metadata only and summarize the effect of the current filters. */
    static bool preview(const std::vector<std::string> &sourcePaths, const FilterOptions &filter,
                        BackupPreview &preview, std::string &error);

    /** Read a v1/v2 archive. v2 encrypted archives require a password. */
    static bool readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error);
    static bool readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error, const std::string &password);

    /** Unpack a v1/v2 archive. v2 encrypted archives require a password. */
    static bool unpack(const std::string &archiveFile, const std::string &destDir, std::string &error);
    static bool unpack(const std::string &archiveFile, const std::string &destDir,
                       std::string &error, const std::string &password);
    static bool unpack(const std::string &archiveFile, const std::string &destDir,
                       std::string &error, const std::string &password, const OperationContext &context);
};

#endif  // PACKER_H_

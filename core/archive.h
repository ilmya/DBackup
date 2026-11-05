// Copyright 2026 BackupTool Team. All rights reserved.

#ifndef DBACKUP_ARCHIVE_ARCHIVE_H_
#define DBACKUP_ARCHIVE_ARCHIVE_H_

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

struct SourceSelectionRule {
    std::string path;
    bool include = false;
};

/** Optional criteria used while scanning a source directory. Empty/zero values mean "not set". */
struct FilterOptions {
    std::string pathContains;
    std::string nameContains;
    std::string extension;
    std::vector<std::string> extensions;
    std::string ownerContains;
    std::vector<PathFilterRule> pathRules;
    std::vector<SourceSelectionRule> sourceRules;
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

enum class PackFormat : uint8_t { Custom = 0, Tar = 1 };
enum class Compression : uint8_t { None = 0, Huffman = 1, Lz77 = 2, Deflate = 3 };

struct PackOptions {
    PackFormat format = PackFormat::Custom;
    Compression compression = Compression::Deflate;
    int compressionLevel = 6;  // zlib level (0..9)
    std::string password;       // empty means no encryption
    FilterOptions filter;
};

struct ArchiveEntry {
    uint8_t     type;           // 0 file, 1 directory, 2 symlink, 3 hard link, 4 FIFO, 5 char, 6 block, 7 socket
    uint8_t     flags = 0;      // bit 0 = zlib-compressed payload
    std::string relativePath;   // UTF-8 relative path
    uint32_t    mode = 0;       // POSIX file mode
    uint32_t    uid = 0;
    uint32_t    gid = 0;
    uint32_t    deviceMajor = 0;
    uint32_t    deviceMinor = 0;
    int64_t     mtimeMs = 0;    // modification time in milliseconds
    std::string owner;          // UTF-8 owner name, if available
    std::string linkTarget;     // symbolic-link or hard-link target
    uint64_t    originalSize = 0;
    std::string data;           // decoded file data; empty for directories
};

/** Parse the GUI filter expression. Returns false for unknown keys or invalid values. */
bool ParseFilterOptions(const std::string &spec, FilterOptions &filter, std::string &error);
bool ValidateFilterOptions(const FilterOptions &filter, std::string &error);

class ArchiveService {
 public:
    /** Create one current-format archive from a file or directory. */
    static bool create(const std::string &sourcePath, const std::string &destFile, std::string &error);

    /** Create an archive with compression, optional password encryption and filters. */
    static bool create(const std::string &sourcePath, const std::string &destFile,
                     const PackOptions &options, std::string &error);

    /** Create one archive from multiple files and/or directories. */
    static bool create(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                     const PackOptions &options, std::string &error);
    static bool create(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                     const PackOptions &options, std::string &error, const OperationContext &context);

    /** Scan metadata only and summarize the effect of the current filters. */
    static bool preview(const std::vector<std::string> &sourcePaths, const FilterOptions &filter,
                        BackupPreview &preview, std::string &error, bool includeSingleDirectoryRoot = false);

    /** Read the current .abk format. Encrypted archives require a password. */
    static bool inspect(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error);
    static bool inspect(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                            std::string &error, const std::string &password);

    /** Extract the current .abk format. Encrypted archives require a password. */
    static bool extract(const std::string &archiveFile, const std::string &destDir, std::string &error);
    static bool extract(const std::string &archiveFile, const std::string &destDir,
                       std::string &error, const std::string &password);
    static bool extract(const std::string &archiveFile, const std::string &destDir,
                       std::string &error, const std::string &password, const OperationContext &context);
};

#endif  // DBACKUP_ARCHIVE_ARCHIVE_H_

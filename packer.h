// Copyright 2026 BackupTool Team. All rights reserved.

/**
 * packer.h — 自定义 .abk (Archive Backup) 二进制格式的打包与解包核心
 *
 * ═══════════════════════════════════════════════════════════
 *  .abk 文件格式（小端序 Little-Endian）
 * ═══════════════════════════════════════════════════════════
 *
 *  ┌─────────────────────────────────────────────┐
 *  │  Header (20 bytes)                          │
 *  │    Magic       : 6 bytes  "ABKPKG"          │
 *  │    Version     : uint16   (当前 = 1)        │
 *  │    EntryCount  : uint32                     │
 *  │    Reserved    : 8 bytes  (全零)            │
 *  ├─────────────────────────────────────────────┤
 *  │  Entry 0                                    │
 *  │    FileType    : uint8    (0=文件, 1=目录)  │
 *  │    PathLen     : uint32                     │
 *  │    Path        : PathLen bytes (UTF-8)      │
 *  │    Mode        : uint32   (chmod 权限)      │
 *  │    Mtime       : int64    (ms since epoch)  │
 *  │    DataSize    : uint64                     │
 *  │    Data        : DataSize bytes             │
 *  ├─────────────────────────────────────────────┤
 *  │  Entry 1 ...                                │
 *  └─────────────────────────────────────────────┘
 */

#ifndef PACKER_H_
#define PACKER_H_

#include <string>
#include <vector>
#include <cstdint>

struct ArchiveEntry {
    uint8_t     type;           // 0 = 文件, 1 = 目录
    std::string relativePath;   // 相对路径 (UTF-8)
    uint32_t    mode;           // chmod 权限
    int64_t     mtimeMs;        // 修改时间（毫秒时间戳）
    std::string data;           // 文件数据（目录为空）
};

class Packer {
 public:
    /**
     * 将 sourceDir 打包为 .abk 文件
     * @return true 成功, false 失败
     */
    static bool pack(const std::string &sourceDir, const std::string &destFile, std::string &error);

    /**
     * 从 .abk 文件读取所有条目
     */
    static bool readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries, std::string &error);

    /**
     * 将 .abk 文件解包到目标目录
     */
    static bool unpack(const std::string &archiveFile, const std::string &destDir, std::string &error);
};

#endif  // PACKER_H_

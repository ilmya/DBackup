// Copyright 2026 BackupTool Team. All rights reserved.

/**
 * packer.cpp — 自定义 .abk 格式的打包与解包核心
 * 使用 Win32 API 进行目录遍历，标准 C++ fstream 进行二进制读写
 */

#include "packer.h"

#include <windows.h>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static const char MAGIC[6] = {'A', 'B', 'K', 'P', 'K', 'G'};
static const uint16_t FORMAT_VERSION = 1;

// ═══════════════════ 小端序读写工具 ═══════════════════

static void writeU8(std::ofstream &out, uint8_t v)   { out.write(reinterpret_cast<const char*>(&v), 1); }
static void writeU16(std::ofstream &out, uint16_t v)  { out.write(reinterpret_cast<const char*>(&v), 2); }
static void writeU32(std::ofstream &out, uint32_t v)  { out.write(reinterpret_cast<const char*>(&v), 4); }
static void writeU64(std::ofstream &out, uint64_t v)  { out.write(reinterpret_cast<const char*>(&v), 8); }
static void writeI64(std::ofstream &out, int64_t v)   { out.write(reinterpret_cast<const char*>(&v), 8); }

static uint8_t  readU8(std::ifstream &in)  { uint8_t  v; in.read(reinterpret_cast<char*>(&v), 1); return v; }
static uint16_t readU16(std::ifstream &in) { uint16_t v; in.read(reinterpret_cast<char*>(&v), 2); return v; }
static uint32_t readU32(std::ifstream &in) { uint32_t v; in.read(reinterpret_cast<char*>(&v), 4); return v; }
static uint64_t readU64(std::ifstream &in) { uint64_t v; in.read(reinterpret_cast<char*>(&v), 8); return v; }
static int64_t  readI64(std::ifstream &in) { int64_t  v; in.read(reinterpret_cast<char*>(&v), 8); return v; }

// ═══════════════════ 字符串转换 (UTF-8 ↔ UTF-16) ═══════════════════

static std::string WtoU(const std::wstring &ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring UtoW(const std::string &s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

// ═══════════════════ 路径拼接 ═══════════════════

static std::string JoinPath(const std::string &base, const std::string &name) {
    if (base.empty()) return name;
    return base + "/" + name;
}

// ═══════════════════ FILETIME → 毫秒时间戳 ═══════════════════

static int64_t FileTimeToMs(const FILETIME &ft) {
    ULARGE_INTEGER ul;
    ul.LowPart  = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    // FILETIME 单位: 100ns 间隔 (自 1601-01-01)
    // Unix epoch 自 1970-01-01，差值 11644473600 秒
    int64_t us100 = static_cast<int64_t>(ul.QuadPart);
    int64_t ms = (us100 - 116444736000000000LL) / 10000;
    return ms;
}

static FILETIME MsToFileTime(int64_t ms) {
    int64_t us100 = ms * 10000 + 116444736000000000LL;
    ULARGE_INTEGER ul;
    ul.QuadPart = static_cast<uint64_t>(us100);
    FILETIME ft;
    ft.dwLowDateTime  = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;
    return ft;
}

// ═══════════════════ Win32 目录遍历 ═══════════════════

struct DirItem {
    std::string name;     // UTF-8 文件名
    bool        isDir;
    int64_t     mtimeMs;
};

/** 读取一个目录下的所有条目（不含 . 和 ..），按名称排序 */
static std::vector<DirItem> ListDirectory(const std::string &dirUtf8) {
    std::vector<DirItem> items;
    std::wstring dirW = UtoW(dirUtf8);
    std::wstring pattern = dirW + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return items;

    do {
        std::wstring nameW(fd.cFileName);
        if (nameW == L"." || nameW == L"..") continue;

        DirItem item;
        item.name   = WtoU(nameW);
        item.isDir  = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.mtimeMs = FileTimeToMs(fd.ftLastWriteTime);
        items.push_back(std::move(item));
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // 按名称排序
    std::sort(items.begin(), items.end(),
        [](const DirItem &a, const DirItem &b) { return a.name < b.name; });

    return items;
}

/** 递归创建目录 (类似 mkdir -p) */
static bool CreateDirRecursive(const std::string &dirUtf8) {
    std::wstring dirW = UtoW(dirUtf8);
    // 逐级创建
    for (size_t i = 1; i < dirW.size(); i++) {
        if (dirW[i] == L'\\' || dirW[i] == L'/') {
            dirW[i] = L'\\';
            CreateDirectoryW(dirW.substr(0, i).c_str(), nullptr);
        }
    }
    return CreateDirectoryW(dirW.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/** 读取整个文件内容 */
static std::string ReadWholeFile(const std::string &pathUtf8) {
    std::ifstream f(pathUtf8, std::ios::binary);
    if (!f.is_open()) return "";
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();
    return data;
}

// ═══════════════════ 递归收集条目 ═══════════════════

static void collectEntries(const std::string &dirUtf8, const std::string &basePath,
                           std::vector<ArchiveEntry> &entries) {
    auto items = ListDirectory(dirUtf8);

    for (const auto &item : items) {
        std::string relPath = JoinPath(basePath, item.name);

        if (item.isDir) {
            ArchiveEntry e;
            e.type = 1;
            e.relativePath = relPath;
            e.mode = 0;
            e.mtimeMs = item.mtimeMs;
            entries.push_back(std::move(e));

            // 递归进入子目录
            std::string subDir = dirUtf8 + "\\" + item.name;
            collectEntries(subDir, relPath, entries);
        } else {
            std::string fullPath = dirUtf8 + "\\" + item.name;
            std::string data = ReadWholeFile(fullPath);

            ArchiveEntry e;
            e.type = 0;
            e.relativePath = relPath;
            e.mode = 0;
            e.mtimeMs = item.mtimeMs;
            e.data = std::move(data);
            entries.push_back(std::move(e));
        }
    }
}

// ═══════════════════ 打包 (Pack) ═══════════════════

bool Packer::pack(const std::string &sourceDir, const std::string &destFile, std::string &error) {
    // 检查源目录是否存在
    DWORD attr = GetFileAttributesW(UtoW(sourceDir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "源目录不存在: " + sourceDir;
        return false;
    }

    // 1. 收集所有条目
    std::vector<ArchiveEntry> entries;
    collectEntries(sourceDir, "", entries);

    // 2. 写入 .abk 文件
    std::ofstream out(destFile, std::ios::binary);
    if (!out.is_open()) {
        error = "无法创建备份文件: " + destFile;
        return false;
    }

    // ── 文件头 (20 bytes) ──
    out.write(MAGIC, 6);                     // magic
    writeU16(out, FORMAT_VERSION);            // version
    writeU32(out, static_cast<uint32_t>(entries.size()));  // entryCount
    for (int i = 0; i < 8; i++) { writeU8(out, 0); }       // reserved  // NOLINT

    // ── 每个条目 ──
    for (const auto &entry : entries) {
        const std::string &pathUtf8 = entry.relativePath;

        writeU8(out, entry.type);                                      // fileType
        writeU32(out, static_cast<uint32_t>(pathUtf8.size()));          // pathLen
        out.write(pathUtf8.c_str(), pathUtf8.size());                   // path
        writeU32(out, entry.mode);                                      // mode
        writeI64(out, entry.mtimeMs);                                   // mtime
        writeU64(out, static_cast<uint64_t>(entry.data.size()));        // dataSize
        if (!entry.data.empty()) {
            out.write(entry.data.c_str(), entry.data.size());           // data
        }
    }

    out.close();
    return true;
}

// ═══════════════════ 读取归档 ═══════════════════

bool Packer::readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries, std::string &error) {
    std::ifstream in(archiveFile, std::ios::binary);
    if (!in.is_open()) {
        error = "无法打开备份文件: " + archiveFile;
        return false;
    }

    // ── 校验文件头 ──
    char magic[6];
    in.read(magic, 6);
    if (memcmp(magic, MAGIC, 6) != 0) {
        error = "不是有效的 .abk 备份文件（Magic 不匹配）";
        in.close();
        return false;
    }

    uint16_t version = readU16(in);
    uint32_t entryCount = readU32(in);

    // 跳过 reserved 8 bytes
    for (int i = 0; i < 8; i++) readU8(in);

    // ── 读取每个条目 ──
    entries.clear();
    for (uint32_t i = 0; i < entryCount; i++) {
        ArchiveEntry entry;
        entry.type = readU8(in);
        uint32_t pathLen = readU32(in);

        std::string pathBuf(pathLen, '\0');
        in.read(pathBuf.data(), pathLen);
        entry.relativePath = pathBuf;

        entry.mode    = readU32(in);
        entry.mtimeMs = readI64(in);
        uint64_t dataSize = readU64(in);

        if (dataSize > 0) {
            entry.data.resize(static_cast<size_t>(dataSize));
            in.read(entry.data.data(), dataSize);
        }

        entries.push_back(std::move(entry));
    }

    in.close();
    return true;
}

// ═══════════════════ 解包 (Unpack) ═══════════════════

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir, std::string &error) {
    // 1. 读取归档
    std::vector<ArchiveEntry> entries;
    if (!readArchive(archiveFile, entries, error)) {
        return false;
    }

    // 2. 确保输出目录存在
    CreateDirRecursive(destDir);

    // 3. 逐个还原
    for (const auto &entry : entries) {
        std::string fullPath = destDir + "\\" + entry.relativePath;
        // 将路径中的 / 替换为 \  (NOLINT)
        for (size_t ci = 0; ci < fullPath.size(); ci++) { if (fullPath[ci] == '/') fullPath[ci] = '\\'; }  // NOLINT

        if (entry.type == 1) {
            // 目录
            CreateDirRecursive(fullPath);
        } else {
            // 文件：先确保父目录存在
            size_t lastSlash = fullPath.rfind('\\');
            if (lastSlash != std::string::npos) {
                CreateDirRecursive(fullPath.substr(0, lastSlash));
            }

            std::ofstream outFile(fullPath, std::ios::binary);
            if (!outFile.is_open()) {
                error = "无法创建文件: " + fullPath;
                return false;
            }
            if (!entry.data.empty()) {
                outFile.write(entry.data.c_str(), entry.data.size());
            }
            outFile.close();
        }

        // 恢复修改时间 (mtime)
        HANDLE hFile = CreateFileW(
            UtoW(fullPath).c_str(),
            FILE_WRITE_ATTRIBUTES,
            0, nullptr, OPEN_EXISTING,
            (entry.type == 1) ? FILE_FLAG_BACKUP_SEMANTICS : 0,
            nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ft = MsToFileTime(entry.mtimeMs);
            SetFileTime(hFile, nullptr, nullptr, &ft);
            CloseHandle(hFile);
        }
    }

    return true;
}

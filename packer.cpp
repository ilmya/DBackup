// Copyright 2026 BackupTool Team. All rights reserved.

/**
 * The archive engine.  Version 1 is kept for backwards compatibility.  Sprint 2
 * archives use version 2: zlib per-file compression, optional AES-256-CBC
 * encryption, a SHA-256 integrity digest, and filtering metadata.
 */

#include "packer.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <zlib.h>
#include <winioctl.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

const char kMagic[6] = {'A', 'B', 'K', 'P', 'K', 'G'};
const uint16_t kVersion1 = 1;
const uint16_t kVersion2 = 2;
const uint16_t kVersion3 = 3;
const uint32_t kEncrypted = 1u;
const uint8_t kCompressed = 1u;

std::string WtoU(const std::wstring &ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), result.data(), n, nullptr, nullptr);
    return result;
}

std::wstring UtoW(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}

std::string JoinPath(const std::string &base, const std::string &name) {
    return base.empty() ? name : base + "/" + name;
}

int64_t FileTimeToMs(const FILETIME &ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return (static_cast<int64_t>(value.QuadPart) - 116444736000000000LL) / 10000;
}

FILETIME MsToFileTime(int64_t ms) {
    ULARGE_INTEGER value;
    value.QuadPart = static_cast<uint64_t>(ms * 10000LL + 116444736000000000LL);
    FILETIME ft;
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    return ft;
}

void AppendU8(std::vector<uint8_t> &out, uint8_t value) { out.push_back(value); }

void AppendU32(std::vector<uint8_t> &out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void AppendU64(std::vector<uint8_t> &out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void AppendI64(std::vector<uint8_t> &out, int64_t value) {
    AppendU64(out, static_cast<uint64_t>(value));
}

bool Take(const std::vector<uint8_t> &in, size_t &pos, void *dst, size_t size) {
    if (pos > in.size() || size > in.size() - pos) return false;
    std::memcpy(dst, in.data() + pos, size);
    pos += size;
    return true;
}

bool TakeU8(const std::vector<uint8_t> &in, size_t &pos, uint8_t &value) {
    return Take(in, pos, &value, 1);
}

bool TakeU16(const std::vector<uint8_t> &in, size_t &pos, uint16_t &value) {
    uint8_t b[2];
    if (!Take(in, pos, b, sizeof(b))) return false;
    value = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
    return true;
}

bool TakeU32(const std::vector<uint8_t> &in, size_t &pos, uint32_t &value) {
    uint8_t b[4];
    if (!Take(in, pos, b, sizeof(b))) return false;
    value = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
            (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool TakeU64(const std::vector<uint8_t> &in, size_t &pos, uint64_t &value) {
    uint8_t b[8];
    if (!Take(in, pos, b, sizeof(b))) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(b[i]) << (i * 8);
    return true;
}

bool TakeI64(const std::vector<uint8_t> &in, size_t &pos, int64_t &value) {
    uint64_t raw;
    if (!TakeU64(in, pos, raw)) return false;
    value = static_cast<int64_t>(raw);
    return true;
}

bool WriteU16(std::ofstream &out, uint16_t value) {
    uint8_t b[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    out.write(reinterpret_cast<const char *>(b), sizeof(b));
    return static_cast<bool>(out);
}

bool WriteU32(std::ofstream &out, uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(value >> (i * 8));
    out.write(reinterpret_cast<const char *>(b), sizeof(b));
    return static_cast<bool>(out);
}

bool ReadWholeFile(const std::string &path, std::string &data, std::string &error) {
    std::ifstream file(std::filesystem::path(UtoW(path)), std::ios::binary);
    if (!file) {
        error = "无法读取文件: " + path;
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        error = "读取文件失败: " + path;
        return false;
    }
    return true;
}

std::wstring AbsolutePath(const std::string &path) {
    std::wstring wide = UtoW(path);
    DWORD size = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (size == 0) return wide;
    std::wstring result(size, L'\0');
    DWORD written = GetFullPathNameW(wide.c_str(), size, result.data(), nullptr);
    if (written == 0 || written >= size) return wide;
    result.resize(written);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return result;
}

std::string TemporaryPath(const std::string &path) {
    return path + ".tmp." + std::to_string(GetCurrentProcessId()) + "." +
           std::to_string(GetCurrentThreadId());
}

bool ReplaceFile(const std::string &temporary, const std::string &destination, std::string &error) {
    if (!MoveFileExW(UtoW(temporary).c_str(), UtoW(destination).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(UtoW(temporary).c_str());
        error = "无法提交输出文件: " + destination;
        return false;
    }
    return true;
}

bool WriteWholeFileAtomic(const std::string &path, const std::string &data, std::string &error) {
    const std::string temporary = TemporaryPath(path);
    std::ofstream file(std::filesystem::path(UtoW(temporary)), std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "无法创建文件: " + path;
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file) {
        file.close();
        DeleteFileW(UtoW(temporary).c_str());
        error = "写入文件失败: " + path;
        return false;
    }
    file.close();
    if (!file) {
        DeleteFileW(UtoW(temporary).c_str());
        error = "写入文件失败: " + path;
        return false;
    }
    return ReplaceFile(temporary, path, error);
}

std::string OwnerName(const std::wstring &path) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                              &sid, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
        return {};
    }
    DWORD nameSize = 0;
    DWORD domainSize = 0;
    SID_NAME_USE use;
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    std::vector<wchar_t> name(nameSize + 1);
    std::vector<wchar_t> domain(domainSize + 1);
    std::string result;
    if (LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use)) {
        result = WtoU(std::wstring(name.data(), nameSize));
    }
    if (descriptor) LocalFree(descriptor);
    return result;
}

struct DirItem {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    int64_t mtimeMs = 0;
    std::string owner;
    uint32_t attributes = 0;
    bool isReparse = false;
};

bool ListDirectory(const std::string &dir, std::vector<DirItem> &items, std::string &error) {
    items.clear();
    std::wstring pattern = UtoW(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "无法读取目录: " + dir;
        return false;
    }
    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        DirItem item;
        item.name = WtoU(name);
        item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.mtimeMs = FileTimeToMs(fd.ftLastWriteTime);
        item.owner = OwnerName(UtoW(dir + "\\" + item.name));
        item.attributes = fd.dwFileAttributes;
        item.isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        items.push_back(std::move(item));
    } while (FindNextFileW(handle, &fd));
    DWORD findError = GetLastError();
    FindClose(handle);
    if (findError != ERROR_NO_MORE_FILES) {
        error = "扫描目录时发生错误: " + dir;
        return false;
    }
    std::sort(items.begin(), items.end(), [](const DirItem &a, const DirItem &b) { return a.name < b.name; });
    return true;
}

bool CreateDirRecursive(const std::string &dir) {
    std::wstring path = UtoW(dir);
    if (path.empty()) return false;
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == L'/' || path[i] == L'\\') {
            wchar_t saved = path[i];
            path[i] = L'\0';
            if (!path.empty()) CreateDirectoryW(path.c_str(), nullptr);
            path[i] = saved;
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<uint8_t> SecurityDescriptor(const std::wstring &path) {
    DWORD size = 0;
    GetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                     DACL_SECURITY_INFORMATION, nullptr, 0, &size);
    std::vector<uint8_t> descriptor(size);
    if (size != 0 && !GetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION |
        GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), size, &size)) descriptor.clear();
    return descriptor;
}

struct PackerReparseBuffer {
    DWORD tag; USHORT dataLength; USHORT reserved;
    union {
        struct { USHORT substituteOffset, substituteLength, printOffset, printLength; ULONG flags; WCHAR path[1]; } symlink;
        struct { USHORT substituteOffset, substituteLength, printOffset, printLength; WCHAR path[1]; } mount;
        struct { UCHAR data[1]; } generic;
    } value;
};

std::string ReparseTarget(const std::wstring &path) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::vector<uint8_t> data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE); DWORD got = 0;
    bool ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, data.data(),
                              static_cast<DWORD>(data.size()), &got, nullptr) != 0;
    CloseHandle(handle); if (!ok) return {};
    auto *buffer = reinterpret_cast<PackerReparseBuffer *>(data.data());
    const WCHAR *text = nullptr; USHORT offset = 0, length = 0;
    if (buffer->tag == IO_REPARSE_TAG_SYMLINK) { text=buffer->value.symlink.path; offset=buffer->value.symlink.printOffset; length=buffer->value.symlink.printLength; }
    else if (buffer->tag == IO_REPARSE_TAG_MOUNT_POINT) { text=buffer->value.mount.path; offset=buffer->value.mount.printOffset; length=buffer->value.mount.printLength; }
    else return {};
    return WtoU(std::wstring(reinterpret_cast<const WCHAR *>(reinterpret_cast<const uint8_t *>(text)+offset), length/sizeof(WCHAR)));
}

bool WildcardMatch(const std::string &patternValue, const std::string &textValue) {
    std::string pattern = Lower(patternValue);
    std::string text = Lower(textValue);
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    std::replace(text.begin(), text.end(), '\\', '/');
    size_t p = 0, t = 0, star = std::string::npos, retry = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            while (p < pattern.size() && pattern[p] == '*') ++p;
            star = p;
            retry = t;
        } else if (star != std::string::npos) {
            p = star;
            t = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool Matches(const DirItem &item, const std::string &relativePath, const FilterOptions &filter) {
    if (filter.type == EntryTypeFilter::FilesOnly && item.isDir) return false;
    if (filter.type == EntryTypeFilter::DirectoriesOnly && !item.isDir) return false;
    const std::string path = Lower(relativePath);
    const std::string name = Lower(item.name);
    bool hasIncludeRule = false;
    bool includedByRule = false;
    for (const auto &rule : filter.pathRules) {
        if (rule.include) hasIncludeRule = true;
        if (!WildcardMatch(rule.pattern, path)) continue;
        if (!rule.include) return false;
        includedByRule = true;
    }
    if (hasIncludeRule && !includedByRule) return false;
    if (!filter.pathContains.empty() && path.find(Lower(filter.pathContains)) == std::string::npos) return false;
    if (!filter.nameContains.empty() && name.find(Lower(filter.nameContains)) == std::string::npos) return false;
    if (!filter.extension.empty() && !item.isDir) {
        std::string ext = Lower(filter.extension);
        if (ext[0] != '.') ext.insert(ext.begin(), '.');
        if (name.size() < ext.size() || name.substr(name.size() - ext.size()) != ext) return false;
    }
    if (!filter.extensions.empty() && !item.isDir) {
        bool extensionMatch = false;
        for (std::string ext : filter.extensions) {
            ext = Lower(ext);
            if (!ext.empty() && ext[0] != '.') ext.insert(ext.begin(), '.');
            if (!ext.empty() && name.size() >= ext.size() &&
                name.substr(name.size() - ext.size()) == ext) {
                extensionMatch = true;
                break;
            }
        }
        if (!extensionMatch) return false;
    }
    if (!filter.ownerContains.empty() && Lower(item.owner).find(Lower(filter.ownerContains)) == std::string::npos) return false;
    if (filter.minSize != 0 && item.size < filter.minSize) return false;
    if (filter.maxSize != 0 && item.size > filter.maxSize) return false;
    if (filter.afterMtimeMs != 0 && item.mtimeMs < filter.afterMtimeMs) return false;
    if (filter.beforeMtimeMs != 0 && item.mtimeMs > filter.beforeMtimeMs) return false;
    return true;
}

bool CollectEntries(const std::string &dir, const std::string &base, const FilterOptions &filter,
                    const std::wstring &excludedPath, std::vector<ArchiveEntry> &entries,
                    std::string &error) {
    std::vector<DirItem> items;
    if (!ListDirectory(dir, items, error)) return false;
    for (const auto &item : items) {
        const std::string sourcePath = dir + "\\" + item.name;
        if (!excludedPath.empty() && AbsolutePath(sourcePath) == excludedPath) continue;
        const std::string relative = JoinPath(base, item.name);
        if (Matches(item, relative, filter)) {
            ArchiveEntry entry;
            entry.type = item.isReparse ? 2 : (item.isDir ? 1 : 0);
            entry.relativePath = relative;
            entry.mtimeMs = item.mtimeMs;
            entry.owner = item.owner;
            entry.securityDescriptor = SecurityDescriptor(UtoW(sourcePath));
            if (item.isReparse) entry.linkTarget = ReparseTarget(UtoW(sourcePath));
            entry.mode = item.attributes;
            entry.originalSize = item.isDir ? 0 : item.size;
            if (!item.isDir && !item.isReparse && !ReadWholeFile(sourcePath, entry.data, error)) return false;
            entries.push_back(std::move(entry));
        }
        if (item.isDir && !item.isReparse && !CollectEntries(sourcePath, relative, filter, excludedPath, entries, error)) return false;
    }
    return true;
}

bool CollectSource(const std::string &sourcePath, const std::string &destFile,
                   const FilterOptions &filter, bool includeDirectoryRoot,
                   std::vector<ArchiveEntry> &entries, std::string &error) {
    const std::wstring sourceWide = UtoW(sourcePath);
    DWORD attributes = GetFileAttributesW(sourceWide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "备份源不存在: " + sourcePath;
        return false;
    }
    if (AbsolutePath(sourcePath) == AbsolutePath(destFile)) {
        error = "备份源文件不能同时作为备份输出文件";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        ArchiveEntry entry; entry.type=2; entry.relativePath=WtoU(sourceWide.substr(sourceWide.find_last_of(L"\\/")+1));
        entry.mode=attributes; entry.owner=OwnerName(sourceWide); entry.securityDescriptor=SecurityDescriptor(sourceWide);
        entry.linkTarget=ReparseTarget(sourceWide); entries.push_back(std::move(entry)); return true;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        size_t slash = sourceWide.find_last_of(L"\\/");
        std::string rootName = WtoU(slash == std::wstring::npos ? sourceWide : sourceWide.substr(slash + 1));
        if (includeDirectoryRoot) {
            WIN32_FILE_ATTRIBUTE_DATA rootData = {};
            if (!GetFileAttributesExW(sourceWide.c_str(), GetFileExInfoStandard, &rootData)) {
                error = "无法读取目录元数据: " + sourcePath;
                return false;
            }
            ArchiveEntry rootEntry;
            rootEntry.type = 1;
            rootEntry.relativePath = rootName;
            rootEntry.mode = rootData.dwFileAttributes;
            rootEntry.mtimeMs = FileTimeToMs(rootData.ftLastWriteTime);
            rootEntry.owner = OwnerName(sourceWide);
            rootEntry.securityDescriptor = SecurityDescriptor(sourceWide);
            entries.push_back(std::move(rootEntry));
        }
        return CollectEntries(sourcePath, includeDirectoryRoot ? rootName : "", filter,
                              AbsolutePath(destFile), entries, error);
    }

    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(sourceWide.c_str(), GetFileExInfoStandard, &data)) {
        error = "无法读取文件信息: " + sourcePath;
        return false;
    }
    size_t slash = sourceWide.find_last_of(L"\\/");
    std::string name = WtoU(slash == std::wstring::npos ? sourceWide : sourceWide.substr(slash + 1));
    DirItem item;
    item.name = name;
    item.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    item.mtimeMs = FileTimeToMs(data.ftLastWriteTime);
    item.owner = OwnerName(sourceWide);
    item.attributes = data.dwFileAttributes;
    if (!Matches(item, name, filter)) return true;

    ArchiveEntry entry;
    entry.type = 0;
    entry.relativePath = name;
    entry.mode = item.attributes;
    entry.mtimeMs = item.mtimeMs;
    entry.owner = item.owner;
    entry.securityDescriptor = SecurityDescriptor(sourceWide);
    entry.originalSize = item.size;
    if (!ReadWholeFile(sourcePath, entry.data, error)) return false;
    entries.push_back(std::move(entry));
    return true;
}

bool CollectSources(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                    const FilterOptions &filter, std::vector<ArchiveEntry> &entries,
                    std::string &error) {
    if (sourcePaths.empty()) {
        error = "请至少选择一个备份来源";
        return false;
    }
    const bool multiple = sourcePaths.size() > 1;
    std::unordered_set<std::wstring> uniqueSources;
    for (const auto &sourcePath : sourcePaths) {
        const std::wstring absolute = AbsolutePath(sourcePath);
        if (!uniqueSources.insert(absolute).second) continue;
        if (!CollectSource(sourcePath, destFile, filter, multiple, entries, error)) return false;
    }
    std::unordered_set<std::string> archivePaths;
    for (const auto &entry : entries) {
        std::string normalized = Lower(entry.relativePath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (!archivePaths.insert(normalized).second) {
            error = "多个备份来源在归档中产生同名路径: " + entry.relativePath;
            return false;
        }
    }
    return true;
}

void AddPreviewItem(const DirItem &item, bool included, BackupPreview &preview) {
    if (item.isDir) {
        if (included) ++preview.includedDirectories;
        else ++preview.excludedDirectories;
    } else if (included) {
        ++preview.includedFiles;
        preview.includedBytes += item.size;
    } else {
        ++preview.excludedFiles;
        preview.excludedBytes += item.size;
    }
}

bool PreviewDirectory(const std::string &dir, const std::string &base, const FilterOptions &filter,
                      BackupPreview &preview, std::string &error) {
    std::vector<DirItem> items;
    if (!ListDirectory(dir, items, error)) return false;
    for (const auto &item : items) {
        const std::string relative = JoinPath(base, item.name);
        AddPreviewItem(item, Matches(item, relative, filter), preview);
        if (item.isDir && !PreviewDirectory(dir + "\\" + item.name, relative, filter, preview, error)) {
            return false;
        }
    }
    return true;
}

bool PreviewSource(const std::string &sourcePath, const FilterOptions &filter, bool includeRoot,
                   BackupPreview &preview, std::string &error) {
    const std::wstring sourceWide = UtoW(sourcePath);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(sourceWide.c_str(), GetFileExInfoStandard, &data)) {
        error = "备份源不存在或无法读取: " + sourcePath;
        return false;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "暂不支持符号链接或重解析点: " + sourcePath;
        return false;
    }
    size_t slash = sourceWide.find_last_of(L"\\/");
    std::string name = WtoU(slash == std::wstring::npos ? sourceWide : sourceWide.substr(slash + 1));
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return PreviewDirectory(sourcePath, includeRoot ? name : "", filter, preview, error);
    }
    DirItem item;
    item.name = name;
    item.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    item.mtimeMs = FileTimeToMs(data.ftLastWriteTime);
    item.owner = OwnerName(sourceWide);
    item.attributes = data.dwFileAttributes;
    AddPreviewItem(item, Matches(item, name, filter), preview);
    return true;
}

bool Compress(const std::string &input, int level, std::string &output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > std::numeric_limits<uLong>::max()) return false;
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    std::vector<uint8_t> buffer(bound);
    uLongf size = bound;
    int result = compress2(buffer.data(), &size,
                           reinterpret_cast<const Bytef *>(input.data()),
                           static_cast<uLong>(input.size()), std::clamp(level, 0, 9));
    if (result != Z_OK) return false;
    output.assign(reinterpret_cast<const char *>(buffer.data()), size);
    return true;
}

bool Decompress(const std::string &input, uint64_t expectedSize, std::string &output) {
    if (expectedSize > static_cast<uint64_t>(std::numeric_limits<uLongf>::max())) return false;
    if (input.size() > std::numeric_limits<uLong>::max()) return false;
    uLongf size = static_cast<uLongf>(expectedSize);
    output.resize(static_cast<size_t>(expectedSize));
    int result = uncompress(reinterpret_cast<Bytef *>(output.data()), &size,
                            reinterpret_cast<const Bytef *>(input.data()),
                            static_cast<uLong>(input.size()));
    if (result != Z_OK || size != expectedSize) {
        output.clear();
        return false;
    }
    return true;
}

bool Sha256(const std::vector<uint8_t> &input, std::vector<uint8_t> &digest) {
    if (input.size() > std::numeric_limits<ULONG>::max()) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &resultSize, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<uint8_t> object(objectSize);
    digest.assign(32, 0);
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0) == 0 &&
              BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool DeriveKey(const std::string &password, const std::vector<uint8_t> &salt, std::vector<uint8_t> &key) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    key.assign(32, 0);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }
    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        100000,
        key.data(), static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

bool RandomBytes(std::vector<uint8_t> &bytes) {
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool AesCrypt(const std::vector<uint8_t> &input, const std::vector<uint8_t> &key,
              const std::vector<uint8_t> &iv, bool encrypt, std::vector<uint8_t> &output) {
    if (input.size() > std::numeric_limits<ULONG>::max()) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    if (key.size() != 32 || iv.size() != 16 || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0 ||
        BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<uint8_t> object(objectSize);
    if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(), objectSize,
                                   const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    ULONG required = 0;
    NTSTATUS status;
    if (encrypt) {
        status = BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                               nullptr, const_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()), nullptr, 0,
                               &required, BCRYPT_BLOCK_PADDING);
    } else {
        status = BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                               nullptr, const_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()), nullptr, 0,
                               &required, BCRYPT_BLOCK_PADDING);
    }
    if (status != 0) {
        BCryptDestroyKey(keyHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    output.resize(required);
    ULONG actual = 0;
    std::vector<uint8_t> ivCopy = iv;
    if (encrypt) {
        status = BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr,
                               ivCopy.data(), static_cast<ULONG>(ivCopy.size()), output.data(), required, &actual, BCRYPT_BLOCK_PADDING);
    } else {
        status = BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr,
                               ivCopy.data(), static_cast<ULONG>(ivCopy.size()), output.data(), required, &actual, BCRYPT_BLOCK_PADDING);
    }
    output.resize(actual);
    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

bool AesGcm(const std::vector<uint8_t> &input, const std::vector<uint8_t> &key,
            const std::vector<uint8_t> &nonce, std::vector<uint8_t> &tag,
            bool encrypt, std::vector<uint8_t> &output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0, resultSize = 0, required = 0, actual = 0;
    if (key.size() != 32 || nonce.size() != 12 ||
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0 ||
        BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &resultSize, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<uint8_t> object(objectSize);
    if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(), objectSize,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    if (encrypt) tag.assign(16, 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<PUCHAR>(nonce.data());
    auth.cbNonce = static_cast<ULONG>(nonce.size());
    auth.pbTag = tag.data();
    auth.cbTag = static_cast<ULONG>(tag.size());
    NTSTATUS status = encrypt
        ? BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                        &auth, nullptr, 0, nullptr, 0, &required, 0)
        : BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                        &auth, nullptr, 0, nullptr, 0, &required, 0);
    if (status >= 0) {
        output.resize(required);
        status = encrypt
            ? BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                            &auth, nullptr, 0, output.data(), required, &actual, 0)
            : BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
                            &auth, nullptr, 0, output.data(), required, &actual, 0);
        output.resize(status >= 0 ? actual : 0);
    }
    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
}

bool SerializeEntries(const std::vector<ArchiveEntry> &entries, int compressionLevel,
                      std::vector<uint8_t> &body, std::string &error) {
    body.clear();
    for (const auto &entry : entries) {
        std::string payload = entry.data;
        uint8_t flags = 0;
        if (entry.type == 0 && !entry.data.empty()) {
            std::string compressed;
            if (!Compress(entry.data, compressionLevel, compressed)) {
                error = "文件压缩失败: " + entry.relativePath;
                return false;
            }
            if (compressed.size() < entry.data.size()) {
                payload = std::move(compressed);
                flags |= kCompressed;
            }
        }
        if (entry.relativePath.size() > std::numeric_limits<uint32_t>::max() ||
            entry.owner.size() > std::numeric_limits<uint32_t>::max()) {
            error = "路径或所有者名称过长: " + entry.relativePath;
            return false;
        }
        AppendU8(body, entry.type);
        AppendU8(body, flags);
        AppendU32(body, static_cast<uint32_t>(entry.relativePath.size()));
        body.insert(body.end(), entry.relativePath.begin(), entry.relativePath.end());
        AppendU32(body, entry.mode);
        AppendI64(body, entry.mtimeMs);
        AppendU32(body, static_cast<uint32_t>(entry.owner.size()));
        body.insert(body.end(), entry.owner.begin(), entry.owner.end());
        AppendU32(body, static_cast<uint32_t>(entry.linkTarget.size()));
        body.insert(body.end(), entry.linkTarget.begin(), entry.linkTarget.end());
        AppendU32(body, static_cast<uint32_t>(entry.securityDescriptor.size()));
        body.insert(body.end(), entry.securityDescriptor.begin(), entry.securityDescriptor.end());
        AppendU64(body, entry.type == 0 ? entry.data.size() : 0);
        AppendU64(body, entry.type == 0 ? payload.size() : 0);
        body.insert(body.end(), payload.begin(), payload.end());
    }
    std::vector<uint8_t> digest;
    if (!Sha256(body, digest)) {
        error = "无法计算归档完整性校验值";
        return false;
    }
    body.insert(body.end(), digest.begin(), digest.end());
    return true;
}

bool ParseV2Entries(const std::vector<uint8_t> &body, uint32_t count,
                    std::vector<ArchiveEntry> &entries, std::string &error) {
    if (body.size() < 32) {
        error = "归档数据不完整";
        return false;
    }
    std::vector<uint8_t> expected;
    std::vector<uint8_t> content(body.begin(), body.end() - 32);
    if (!Sha256(content, expected) || !std::equal(expected.begin(), expected.end(), body.end() - 32)) {
        error = "归档校验失败（密码错误或文件已损坏）";
        return false;
    }
    size_t pos = 0;
    entries.clear();
    for (uint32_t i = 0; i < count; ++i) {
        ArchiveEntry entry;
        uint32_t pathSize = 0;
        uint32_t ownerSize = 0;
        uint64_t storedSize = 0;
        if (!TakeU8(body, pos, entry.type) || !TakeU8(body, pos, entry.flags) ||
            !TakeU32(body, pos, pathSize) || pathSize > body.size() - pos) {
            error = "归档条目头部不完整";
            return false;
        }
        if (entry.type > 1 || (entry.flags & ~kCompressed) != 0) {
            error = "归档条目类型或标志非法";
            return false;
        }
        entry.relativePath.assign(reinterpret_cast<const char *>(body.data() + pos), pathSize);
        pos += pathSize;
        if (!TakeU32(body, pos, entry.mode) || !TakeI64(body, pos, entry.mtimeMs) ||
            !TakeU32(body, pos, ownerSize) || ownerSize > body.size() - pos) {
            error = "归档条目元数据不完整";
            return false;
        }
        entry.owner.assign(reinterpret_cast<const char *>(body.data() + pos), ownerSize);
        pos += ownerSize;
        if (!TakeU64(body, pos, entry.originalSize) || !TakeU64(body, pos, storedSize) ||
            storedSize > body.size() - pos || storedSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            error = "归档条目数据长度非法";
            return false;
        }
        std::string payload(reinterpret_cast<const char *>(body.data() + pos), static_cast<size_t>(storedSize));
        pos += static_cast<size_t>(storedSize);
        if (entry.type == 1) {
            if (entry.originalSize != 0 || storedSize != 0) {
                error = "目录条目包含非法数据";
                return false;
            }
        } else if (entry.flags & kCompressed) {
            if (!Decompress(payload, entry.originalSize, entry.data)) {
                error = "文件解压失败: " + entry.relativePath;
                return false;
            }
        } else {
            if (entry.originalSize != storedSize) {
                error = "文件大小校验失败: " + entry.relativePath;
                return false;
            }
            entry.data = std::move(payload);
        }
        entries.push_back(std::move(entry));
    }
    if (pos != body.size() - 32) {
        error = "归档包含多余或截断数据";
        return false;
    }
    return true;
}

bool IsSafeRelativePath(const std::string &path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos) return false;
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::stringstream stream(normalized);
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component == ".." || component.empty()) return false;
    }
    return true;
}

bool ReadFileBytes(const std::string &path, std::vector<uint8_t> &bytes) {
    std::ifstream file(std::filesystem::path(UtoW(path)), std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool ParseV3Entries(const std::vector<uint8_t> &body, uint32_t count,
                    std::vector<ArchiveEntry> &entries, std::string &error) {
    if (body.size() < 32) { error="归档数据不完整"; return false; }
    std::vector<uint8_t> expected, content(body.begin(),body.end()-32);
    if(!Sha256(content,expected)||!std::equal(expected.begin(),expected.end(),body.end()-32)){error="归档校验失败（密码错误或文件已损坏）";return false;}
    size_t pos=0;entries.clear();
    for(uint32_t i=0;i<count;++i){ArchiveEntry entry;uint32_t pathSize=0,ownerSize=0,linkSize=0,securitySize=0;uint64_t storedSize=0;
        if(!TakeU8(body,pos,entry.type)||!TakeU8(body,pos,entry.flags)||!TakeU32(body,pos,pathSize)||pathSize>body.size()-pos){error="归档条目头部不完整";return false;}
        if(entry.type>2||(entry.flags&~kCompressed)!=0){error="归档条目类型或标志非法";return false;}entry.relativePath.assign(reinterpret_cast<const char*>(body.data()+pos),pathSize);pos+=pathSize;
        if(!TakeU32(body,pos,entry.mode)||!TakeI64(body,pos,entry.mtimeMs)||!TakeU32(body,pos,ownerSize)||ownerSize>body.size()-pos){error="归档条目元数据不完整";return false;}entry.owner.assign(reinterpret_cast<const char*>(body.data()+pos),ownerSize);pos+=ownerSize;
        if(!TakeU32(body,pos,linkSize)||linkSize>body.size()-pos){error="链接元数据不完整";return false;}entry.linkTarget.assign(reinterpret_cast<const char*>(body.data()+pos),linkSize);pos+=linkSize;
        if(!TakeU32(body,pos,securitySize)||securitySize>body.size()-pos){error="安全描述符不完整";return false;}entry.securityDescriptor.assign(body.begin()+pos,body.begin()+pos+securitySize);pos+=securitySize;
        if(!TakeU64(body,pos,entry.originalSize)||!TakeU64(body,pos,storedSize)||storedSize>body.size()-pos){error="归档条目数据长度非法";return false;}std::string payload(reinterpret_cast<const char*>(body.data()+pos),static_cast<size_t>(storedSize));pos+=storedSize;
        if(entry.type!=0){if(storedSize||entry.originalSize){error="非文件条目包含数据";return false;}}
        else if(entry.flags&kCompressed){if(!Decompress(payload,entry.originalSize,entry.data)){error="文件解压失败: "+entry.relativePath;return false;}}
        else{if(entry.originalSize!=storedSize){error="文件大小校验失败";return false;}entry.data=std::move(payload);}entries.push_back(std::move(entry));}
    if(pos!=body.size()-32){error="归档包含多余或截断数据";return false;}return true;
}

bool WriteArchiveAtomic(const std::string &destFile,
                        const std::function<bool(std::ofstream &)> &writer,
                        std::string &error) {
    const std::string temporary = TemporaryPath(destFile);
    std::ofstream out(std::filesystem::path(UtoW(temporary)), std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "无法创建备份文件: " + destFile;
        return false;
    }
    if (!writer(out) || !out) {
        out.close();
        DeleteFileW(UtoW(temporary).c_str());
        if (error.empty()) error = "写入备份文件失败: " + destFile;
        return false;
    }
    out.close();
    if (!out) {
        DeleteFileW(UtoW(temporary).c_str());
        error = "写入备份文件失败: " + destFile;
        return false;
    }
    return ReplaceFile(temporary, destFile, error);
}

bool PackLegacy(const std::string &sourcePath, const std::string &destFile, std::string &error) {
    FilterOptions filter;
    std::vector<ArchiveEntry> entries;
    if (!CollectSource(sourcePath, destFile, filter, false, entries, error)) return false;
    return WriteArchiveAtomic(destFile, [&](std::ofstream &out) {
        out.write(kMagic, sizeof(kMagic));
        if (!WriteU16(out, kVersion1) || !WriteU32(out, static_cast<uint32_t>(entries.size()))) return false;
        uint8_t reserved[8] = {};
        out.write(reinterpret_cast<const char *>(reserved), sizeof(reserved));
        for (const auto &entry : entries) {
            uint8_t type = entry.type;
            out.write(reinterpret_cast<const char *>(&type), 1);
            if (!WriteU32(out, static_cast<uint32_t>(entry.relativePath.size()))) return false;
            out.write(entry.relativePath.data(), static_cast<std::streamsize>(entry.relativePath.size()));
            if (!WriteU32(out, entry.mode)) return false;
            uint64_t rawTime = static_cast<uint64_t>(entry.mtimeMs);
            out.write(reinterpret_cast<const char *>(&rawTime), sizeof(rawTime));
            uint64_t dataSize = entry.data.size();
            out.write(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
            out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()));
        }
        return static_cast<bool>(out);
    }, error);
}

}  // namespace

bool ParseFilterOptions(const std::string &spec, FilterOptions &filter, std::string &error) {
    filter = FilterOptions{};
    error.clear();
    auto trim = [](std::string value) {
        const char *spaces = " \t\r\n";
        size_t first = value.find_first_not_of(spaces);
        if (first == std::string::npos) return std::string{};
        size_t last = value.find_last_not_of(spaces);
        return value.substr(first, last - first + 1);
    };
    auto parseSize = [&](const std::string &value, uint64_t &target, const std::string &key) {
        if (value.empty() || value[0] == '-') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        errno = 0;
        char *end = nullptr;
        unsigned long long number = std::strtoull(value.c_str(), &end, 10);
        if (errno == ERANGE || end == value.c_str() || *end != '\0') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        target = static_cast<uint64_t>(number);
        return true;
    };
    auto parseDate = [&](const std::string &value, int64_t &target, const std::string &key, bool endOfDay) {
        std::tm tm = {};
        std::istringstream input(value);
        input >> std::get_time(&tm, "%Y-%m-%d");
        if (input.fail() || input.peek() != std::char_traits<char>::eof()) {
            error = "筛选项 " + key + " 的日期格式应为 YYYY-MM-DD";
            return false;
        }
        const int expectedYear = tm.tm_year;
        const int expectedMonth = tm.tm_mon;
        const int expectedDay = tm.tm_mday;
        time_t seconds = _mkgmtime(&tm);
        if (seconds == static_cast<time_t>(-1)) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        std::tm check = {};
        gmtime_s(&check, &seconds);
        if (check.tm_year != expectedYear || check.tm_mon != expectedMonth || check.tm_mday != expectedDay) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        target = static_cast<int64_t>(seconds) * 1000 + (endOfDay ? 86399999LL : 0LL);
        return true;
    };

    std::stringstream stream(spec);
    std::string part;
    while (std::getline(stream, part, ';')) {
        part = trim(part);
        if (part.empty()) continue;
        size_t equal = part.find('=');
        if (equal == std::string::npos) {
            error = "筛选项缺少等号: " + part;
            return false;
        }
        std::string key = Lower(trim(part.substr(0, equal)));
        std::string value = trim(part.substr(equal + 1));
        if (key.empty() || value.empty()) {
            error = "筛选项的名称和值不能为空: " + part;
            return false;
        }
        if (key == "path") filter.pathContains = value;
        else if (key == "name") filter.nameContains = value;
        else if (key == "ext" || key == "extension") {
            std::stringstream extensions(value);
            std::string extension;
            while (std::getline(extensions, extension, ',')) {
                extension = trim(extension);
                if (!extension.empty()) filter.extensions.push_back(extension);
            }
            if (filter.extensions.empty()) {
                error = "筛选项 ext 至少需要一个扩展名";
                return false;
            }
            if (filter.extensions.size() == 1) filter.extension = filter.extensions.front();
        }
        else if (key == "include" || key == "exclude") {
            filter.pathRules.push_back(PathFilterRule{key == "include", value});
        }
        else if (key == "owner" || key == "user") filter.ownerContains = value;
        else if (key == "type") {
            std::string type = Lower(value);
            if (type == "file" || type == "files") filter.type = EntryTypeFilter::FilesOnly;
            else if (type == "dir" || type == "directory" || type == "directories") {
                filter.type = EntryTypeFilter::DirectoriesOnly;
            } else {
                error = "筛选项 type 只支持 file 或 dir";
                return false;
            }
        } else if (key == "minsize") {
            if (!parseSize(value, filter.minSize, key)) return false;
        } else if (key == "maxsize") {
            if (!parseSize(value, filter.maxSize, key)) return false;
        } else if (key == "after") {
            if (!parseDate(value, filter.afterMtimeMs, key, false)) return false;
        } else if (key == "before") {
            if (!parseDate(value, filter.beforeMtimeMs, key, true)) return false;
        } else {
            error = "未知筛选项: " + key;
            return false;
        }
    }
    if (filter.minSize != 0 && filter.maxSize != 0 && filter.minSize > filter.maxSize) {
        error = "minsize 不能大于 maxsize";
        return false;
    }
    if (filter.afterMtimeMs != 0 && filter.beforeMtimeMs != 0 &&
        filter.afterMtimeMs > filter.beforeMtimeMs) {
        error = "after 不能晚于 before";
        return false;
    }
    return true;
}

bool Packer::pack(const std::string &sourcePath, const std::string &destFile, std::string &error) {
    return PackLegacy(sourcePath, destFile, error);
}

bool Packer::pack(const std::string &sourcePath, const std::string &destFile,
                   const PackOptions &options, std::string &error) {
    return pack(std::vector<std::string>{sourcePath}, destFile, options, error);
}

bool Packer::pack(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                  const PackOptions &options, std::string &error) {
    return pack(sourcePaths, destFile, options, error, OperationContext{});
}

bool Packer::pack(const std::vector<std::string> &sourcePaths, const std::string &destFile,
                  const PackOptions &options, std::string &error, const OperationContext &context) {
    if (context.isCancelled()) { error = "操作已取消"; return false; }
    OperationProgress progress; progress.stage = OperationStage::Scanning; context.report(progress);
    std::vector<ArchiveEntry> entries;
    if (!CollectSources(sourcePaths, destFile, options.filter, entries, error)) return false;
    if (entries.size() > std::numeric_limits<uint32_t>::max()) {
        error = "归档条目数量超出格式限制";
        return false;
    }
    std::vector<uint8_t> body;
    if (!SerializeEntries(entries, options.compressionLevel, body, error)) return false;
    uint32_t flags = options.password.empty() ? 0 : kEncrypted;
    progress.stage = OperationStage::Compressing; progress.totalFiles = entries.size(); context.report(progress);
    std::vector<uint8_t> salt(16), nonce(12), tag, key, storedBody;
    if (flags != 0) {
        progress.stage = OperationStage::Encrypting; context.report(progress);
        if (!RandomBytes(salt) || !RandomBytes(nonce) || !DeriveKey(options.password, salt, key) ||
            !AesGcm(body, key, nonce, tag, true, storedBody)) {
            error = "无法加密归档数据";
            return false;
        }
    } else {
        storedBody = std::move(body);
    }
    return WriteArchiveAtomic(destFile, [&](std::ofstream &out) {
        out.write(kMagic, sizeof(kMagic));
        if (!WriteU16(out, kVersion3) || !WriteU32(out, static_cast<uint32_t>(entries.size())) ||
            !WriteU32(out, flags) || !WriteU32(out, 0)) return false;
        if (flags != 0) {
            out.write(reinterpret_cast<const char *>(salt.data()), static_cast<std::streamsize>(salt.size()));
            out.write(reinterpret_cast<const char *>(nonce.data()), static_cast<std::streamsize>(nonce.size()));
            out.write(reinterpret_cast<const char *>(tag.data()), static_cast<std::streamsize>(tag.size()));
        }
        out.write(reinterpret_cast<const char *>(storedBody.data()), static_cast<std::streamsize>(storedBody.size()));
        return static_cast<bool>(out);
    }, error);
}

bool Packer::preview(const std::vector<std::string> &sourcePaths, const FilterOptions &filter,
                     BackupPreview &preview, std::string &error) {
    preview = BackupPreview{};
    error.clear();
    if (sourcePaths.empty()) {
        error = "请至少选择一个备份来源";
        return false;
    }
    const bool multiple = sourcePaths.size() > 1;
    std::unordered_set<std::wstring> uniqueSources;
    for (const auto &sourcePath : sourcePaths) {
        if (!uniqueSources.insert(AbsolutePath(sourcePath)).second) continue;
        if (!PreviewSource(sourcePath, filter, multiple, preview, error)) return false;
    }
    return true;
}

bool Packer::readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries, std::string &error) {
    return readArchive(archiveFile, entries, error, "");
}

bool Packer::readArchive(const std::string &archiveFile, std::vector<ArchiveEntry> &entries,
                         std::string &error, const std::string &password) {
    std::vector<uint8_t> file;
    if (!ReadFileBytes(archiveFile, file) || file.size() < 20) {
        error = "无法打开或读取备份文件: " + archiveFile;
        return false;
    }
    if (std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "不是有效的 .abk 备份文件（Magic 不匹配）";
        return false;
    }
    size_t pos = 6;
    uint16_t version = 0;
    uint32_t count = 0;
    if (!TakeU16(file, pos, version) || !TakeU32(file, pos, count)) {
        error = "归档头不完整";
        return false;
    }
    if (count > 10000000u) {
        error = "归档条目数量非法";
        return false;
    }
    if (version == kVersion1) {
        if (file.size() < pos + 8) { error = "归档头不完整"; return false; }
        pos += 8;
        entries.clear();
        for (uint32_t i = 0; i < count; ++i) {
            ArchiveEntry entry;
            uint32_t pathSize = 0;
            uint64_t rawTime = 0;
            uint64_t dataSize = 0;
            if (!TakeU8(file, pos, entry.type) || !TakeU32(file, pos, pathSize) || pathSize > file.size() - pos) {
                error = "归档条目头部不完整"; return false;
            }
            if (entry.type > 1) { error = "归档条目类型非法"; return false; }
            entry.relativePath.assign(reinterpret_cast<const char *>(file.data() + pos), pathSize); pos += pathSize;
            if (!TakeU32(file, pos, entry.mode) || !TakeU64(file, pos, rawTime) || !TakeU64(file, pos, dataSize) ||
                dataSize > file.size() - pos || dataSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                error = "归档条目数据非法"; return false;
            }
            entry.mtimeMs = static_cast<int64_t>(rawTime);
            entry.originalSize = dataSize;
            entry.data.assign(reinterpret_cast<const char *>(file.data() + pos), static_cast<size_t>(dataSize));
            pos += static_cast<size_t>(dataSize);
            entries.push_back(std::move(entry));
        }
        if (pos != file.size()) { error = "归档包含多余数据"; return false; }
        return true;
    }
    if ((version != kVersion2 && version != kVersion3) || file.size() < pos + 8) {
        error = "不支持的 .abk 版本";
        return false;
    }
    uint32_t flags = 0;
    uint32_t reserved = 0;
    if (!TakeU32(file, pos, flags) || !TakeU32(file, pos, reserved) || (flags & ~kEncrypted) != 0) {
        error = "归档标志非法";
        return false;
    }
    std::vector<uint8_t> body(file.begin() + static_cast<std::ptrdiff_t>(pos), file.end());
    if (flags & kEncrypted) {
        const size_t cryptoHeader = version == kVersion3 ? 44 : 32;
        if (password.empty() || body.size() < cryptoHeader) {
            error = "该备份文件需要密码";
            return false;
        }
        std::vector<uint8_t> salt(body.begin(), body.begin() + 16);
        std::vector<uint8_t> key;
        bool decrypted = false;
        if (version == kVersion3) {
            std::vector<uint8_t> nonce(body.begin() + 16, body.begin() + 28);
            std::vector<uint8_t> tag(body.begin() + 28, body.begin() + 44);
            std::vector<uint8_t> encrypted(body.begin() + 44, body.end());
            decrypted = DeriveKey(password, salt, key) && AesGcm(encrypted, key, nonce, tag, false, body);
        } else {
            std::vector<uint8_t> iv(body.begin() + 16, body.begin() + 32);
            std::vector<uint8_t> encrypted(body.begin() + 32, body.end());
            decrypted = DeriveKey(password, salt, key) && AesCrypt(encrypted, key, iv, false, body);
        }
        if (!decrypted) {
            error = "密码错误或归档解密失败";
            return false;
        }
    }
    return version == kVersion3 ? ParseV3Entries(body, count, entries, error)
                                : ParseV2Entries(body, count, entries, error);
}

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir, std::string &error) {
    return unpack(archiveFile, destDir, error, "");
}

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir,
                    std::string &error, const std::string &password) {
    return unpack(archiveFile, destDir, error, password, OperationContext{});
}

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir,
                    std::string &error, const std::string &password,
                    const OperationContext &context) {
    if (context.isCancelled()) { error = "操作已取消"; return false; }
    std::vector<ArchiveEntry> entries;
    if (!readArchive(archiveFile, entries, error, password)) return false;
    uint64_t completed = 0;
    for (const auto &entry : entries) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        if (!IsSafeRelativePath(entry.relativePath)) {
            error = "归档包含不安全路径: " + entry.relativePath;
            return false;
        }
        if (entry.type == 2 && !IsSafeRelativePath(entry.linkTarget)) {
            error = "归档包含不安全链接目标: " + entry.linkTarget;
            return false;
        }
    }
    if (!CreateDirRecursive(destDir)) {
        error = "无法创建还原目录: " + destDir;
        return false;
    }
    for (const auto &entry : entries) {
        std::string fullPath = destDir + "\\" + entry.relativePath;
        std::replace(fullPath.begin(), fullPath.end(), '/', '\\');
        if (entry.type == 1) {
            if (!CreateDirRecursive(fullPath)) {
                error = "无法创建目录: " + fullPath;
                return false;
            }
        } else if (entry.type == 2) {
            size_t slash = fullPath.find_last_of('\\');
            if (slash != std::string::npos) CreateDirRecursive(fullPath.substr(0, slash));
            DWORD flags = (entry.mode & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
            flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
            if (!CreateSymbolicLinkW(UtoW(fullPath).c_str(), UtoW(entry.linkTarget).c_str(), flags)) {
                error = "无法恢复符号链接: " + fullPath;
                return false;
            }
        } else {
            size_t slash = fullPath.find_last_of('\\');
            if (slash != std::string::npos && !CreateDirRecursive(fullPath.substr(0, slash))) {
                error = "无法创建父目录: " + fullPath;
                return false;
            }
            if (!WriteWholeFileAtomic(fullPath, entry.data, error)) return false;
        }
        if (entry.type != 1) {
            HANDLE handle = CreateFileW(UtoW(fullPath).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                FILETIME ft = MsToFileTime(entry.mtimeMs);
                SetFileTime(handle, nullptr, nullptr, &ft);
                CloseHandle(handle);
            }
        }
        if (entry.mode != 0 && entry.type != 2 && entry.type != 1) {
            DWORD attributes = entry.mode & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
            if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileAttributesW(UtoW(fullPath).c_str(), attributes)) {
                error = "无法恢复文件属性: " + fullPath;
                return false;
            }
        }
        if (!entry.securityDescriptor.empty() && entry.type != 2 && entry.type != 1) {
            SetFileSecurityW(UtoW(fullPath).c_str(), OWNER_SECURITY_INFORMATION |
                GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(
                    const_cast<uint8_t *>(entry.securityDescriptor.data())));
        }
        OperationProgress progress; progress.stage = OperationStage::Restoring;
        progress.currentPath = entry.relativePath; progress.completedFiles = ++completed;
        progress.totalFiles = entries.size(); context.report(progress);
    }
    // Creating children changes their parent directory's mtime. Restore directory
    // metadata only after the complete tree exists, deepest directory first.
    for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
        const auto &entry = *iterator;
        if (entry.type != 1) continue;
        std::string fullPath = destDir + "\\" + entry.relativePath;
        std::replace(fullPath.begin(), fullPath.end(), '/', '\\');
        HANDLE handle = CreateFileW(UtoW(fullPath).c_str(), FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = "无法打开目录以恢复时间: " + fullPath;
            return false;
        }
        FILETIME ft = MsToFileTime(entry.mtimeMs);
        if (!SetFileTime(handle, nullptr, nullptr, &ft)) {
            CloseHandle(handle); error = "无法恢复目录时间: " + fullPath; return false;
        }
        CloseHandle(handle);
        if (entry.mode != 0) {
            DWORD attributes = entry.mode & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
            if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileAttributesW(UtoW(fullPath).c_str(), attributes)) {
                error = "无法恢复目录属性: " + fullPath; return false;
            }
        }
        if (!entry.securityDescriptor.empty()) {
            SetFileSecurityW(UtoW(fullPath).c_str(), OWNER_SECURITY_INFORMATION |
                GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<uint8_t *>(entry.securityDescriptor.data())));
        }
    }
    OperationProgress done; done.stage = OperationStage::Completed;
    done.completedFiles = entries.size(); done.totalFiles = entries.size(); context.report(done);
    return true;
}

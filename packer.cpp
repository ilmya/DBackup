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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

const char kMagic[6] = {'A', 'B', 'K', 'P', 'K', 'G'};
const uint16_t kVersion1 = 1;
const uint16_t kVersion2 = 2;
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
    if (size > in.size() - pos) return false;
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

std::string ReadWholeFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool WriteWholeFile(const std::string &path, const std::string &data, std::string &error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "无法创建文件: " + path;
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file) {
        error = "写入文件失败: " + path;
        return false;
    }
    return true;
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
};

std::vector<DirItem> ListDirectory(const std::string &dir) {
    std::vector<DirItem> items;
    std::wstring pattern = UtoW(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) return items;
    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        DirItem item;
        item.name = WtoU(name);
        item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.mtimeMs = FileTimeToMs(fd.ftLastWriteTime);
        item.owner = OwnerName(UtoW(dir + "\\" + item.name));
        items.push_back(std::move(item));
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);
    std::sort(items.begin(), items.end(), [](const DirItem &a, const DirItem &b) { return a.name < b.name; });
    return items;
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

bool Matches(const DirItem &item, const std::string &relativePath, const FilterOptions &filter) {
    if (filter.type == EntryTypeFilter::FilesOnly && item.isDir) return false;
    if (filter.type == EntryTypeFilter::DirectoriesOnly && !item.isDir) return false;
    const std::string path = Lower(relativePath);
    const std::string name = Lower(item.name);
    if (!filter.pathContains.empty() && path.find(Lower(filter.pathContains)) == std::string::npos) return false;
    if (!filter.nameContains.empty() && name.find(Lower(filter.nameContains)) == std::string::npos) return false;
    if (!filter.extension.empty() && !item.isDir) {
        std::string ext = Lower(filter.extension);
        if (ext[0] != '.') ext.insert(ext.begin(), '.');
        if (name.size() < ext.size() || name.substr(name.size() - ext.size()) != ext) return false;
    }
    if (!filter.ownerContains.empty() && Lower(item.owner).find(Lower(filter.ownerContains)) == std::string::npos) return false;
    if (filter.minSize != 0 && item.size < filter.minSize) return false;
    if (filter.maxSize != 0 && item.size > filter.maxSize) return false;
    if (filter.afterMtimeMs != 0 && item.mtimeMs < filter.afterMtimeMs) return false;
    if (filter.beforeMtimeMs != 0 && item.mtimeMs > filter.beforeMtimeMs) return false;
    return true;
}

void CollectEntries(const std::string &dir, const std::string &base, const FilterOptions &filter,
                    std::vector<ArchiveEntry> &entries) {
    for (const auto &item : ListDirectory(dir)) {
        const std::string relative = JoinPath(base, item.name);
        if (Matches(item, relative, filter)) {
            ArchiveEntry entry;
            entry.type = item.isDir ? 1 : 0;
            entry.relativePath = relative;
            entry.mtimeMs = item.mtimeMs;
            entry.owner = item.owner;
            entry.originalSize = item.isDir ? 0 : item.size;
            if (!item.isDir) entry.data = ReadWholeFile(dir + "\\" + item.name);
            entries.push_back(std::move(entry));
        }
        if (item.isDir) CollectEntries(dir + "\\" + item.name, relative, filter, entries);
    }
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
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool PackLegacy(const std::string &sourceDir, const std::string &destFile, std::string &error) {
    DWORD attr = GetFileAttributesW(UtoW(sourceDir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "源目录不存在: " + sourceDir;
        return false;
    }
    FilterOptions filter;
    std::vector<ArchiveEntry> entries;
    CollectEntries(sourceDir, "", filter, entries);
    std::ofstream out(destFile, std::ios::binary);
    if (!out) {
        error = "无法创建备份文件: " + destFile;
        return false;
    }
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
    if (!out) {
        error = "写入备份文件失败: " + destFile;
        return false;
    }
    return true;
}

}  // namespace

bool Packer::pack(const std::string &sourceDir, const std::string &destFile, std::string &error) {
    return PackLegacy(sourceDir, destFile, error);
}

bool Packer::pack(const std::string &sourceDir, const std::string &destFile,
                  const PackOptions &options, std::string &error) {
    DWORD attr = GetFileAttributesW(UtoW(sourceDir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "源目录不存在: " + sourceDir;
        return false;
    }
    std::vector<ArchiveEntry> entries;
    CollectEntries(sourceDir, "", options.filter, entries);
    if (entries.size() > std::numeric_limits<uint32_t>::max()) {
        error = "归档条目数量超出格式限制";
        return false;
    }
    std::vector<uint8_t> body;
    if (!SerializeEntries(entries, options.compressionLevel, body, error)) return false;
    uint32_t flags = options.password.empty() ? 0 : kEncrypted;
    std::vector<uint8_t> salt(16), iv(16), key, storedBody;
    if (flags != 0) {
        if (!RandomBytes(salt) || !RandomBytes(iv) || !DeriveKey(options.password, salt, key) ||
            !AesCrypt(body, key, iv, true, storedBody)) {
            error = "无法加密归档数据";
            return false;
        }
    } else {
        storedBody = std::move(body);
    }
    std::ofstream out(destFile, std::ios::binary);
    if (!out) {
        error = "无法创建备份文件: " + destFile;
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    if (!WriteU16(out, kVersion2) || !WriteU32(out, static_cast<uint32_t>(entries.size())) ||
        !WriteU32(out, flags) || !WriteU32(out, 0)) {
        error = "写入归档头失败";
        return false;
    }
    if (flags != 0) {
        out.write(reinterpret_cast<const char *>(salt.data()), static_cast<std::streamsize>(salt.size()));
        out.write(reinterpret_cast<const char *>(iv.data()), static_cast<std::streamsize>(iv.size()));
    }
    out.write(reinterpret_cast<const char *>(storedBody.data()), static_cast<std::streamsize>(storedBody.size()));
    if (!out) {
        error = "写入备份文件失败: " + destFile;
        return false;
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
    if (version != kVersion2 || file.size() < pos + 8) {
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
        if (password.empty() || body.size() < 32) {
            error = "该备份文件需要密码";
            return false;
        }
        std::vector<uint8_t> salt(body.begin(), body.begin() + 16);
        std::vector<uint8_t> iv(body.begin() + 16, body.begin() + 32);
        std::vector<uint8_t> encrypted(body.begin() + 32, body.end());
        std::vector<uint8_t> key;
        if (!DeriveKey(password, salt, key) || !AesCrypt(encrypted, key, iv, false, body)) {
            error = "密码错误或归档解密失败";
            return false;
        }
    }
    return ParseV2Entries(body, count, entries, error);
}

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir, std::string &error) {
    return unpack(archiveFile, destDir, error, "");
}

bool Packer::unpack(const std::string &archiveFile, const std::string &destDir,
                    std::string &error, const std::string &password) {
    std::vector<ArchiveEntry> entries;
    if (!readArchive(archiveFile, entries, error, password)) return false;
    if (!CreateDirRecursive(destDir)) {
        error = "无法创建还原目录: " + destDir;
        return false;
    }
    for (const auto &entry : entries) {
        if (!IsSafeRelativePath(entry.relativePath)) {
            error = "归档包含不安全路径: " + entry.relativePath;
            return false;
        }
        std::string fullPath = destDir + "\\" + entry.relativePath;
        std::replace(fullPath.begin(), fullPath.end(), '/', '\\');
        if (entry.type == 1) {
            if (!CreateDirRecursive(fullPath)) {
                error = "无法创建目录: " + fullPath;
                return false;
            }
        } else {
            size_t slash = fullPath.find_last_of('\\');
            if (slash != std::string::npos && !CreateDirRecursive(fullPath.substr(0, slash))) {
                error = "无法创建父目录: " + fullPath;
                return false;
            }
            if (!WriteWholeFile(fullPath, entry.data, error)) return false;
        }
        HANDLE handle = CreateFileW(UtoW(fullPath).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                    entry.type == 1 ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            FILETIME ft = MsToFileTime(entry.mtimeMs);
            SetFileTime(handle, nullptr, nullptr, &ft);
            CloseHandle(handle);
        }
    }
    return true;
}

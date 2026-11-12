// 实现自定义备份条目的内存编码与解码
// 负责内存形式的自定义格式序列化、摘要校验与可选加密，解码时检查长度和条目字段。
// 序列化文件内容和元数据，校验格式并处理可选加密数据
#include "core/archive/archive.h"
#include "core/archive/archive_internal.h"
#include "core/selection/backup_policy.h"
#include "core/security/crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace dbackup::archive_format {
namespace {
constexpr uint8_t magic[] = {'D', 'B', 'L', 'I', 'N', '0', '0', '2'};
constexpr size_t digestSize = 32, saltSize = 16, nonceSize = 12, tagSize = 16;
constexpr uint64_t maxEntries = 10000000;

// 按小端序写入固定八字节整数，供自定义备份字段编码使用
void put64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
// 写入带长度前缀的字节数组，避免解析时依赖分隔符
void putBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t size) {
    put64(out, size);
    if (size)
        out.insert(out.end(), data, data + size);
}
// 将 UTF-8 字符串写成长度前缀字节字段
void putString(std::vector<uint8_t>& out, const std::string& value) {
    putBytes(out, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}
// 跟踪内存缓冲区的读取位置，解码字段时检查长度边界。
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t offset = 0;
    // 安全读取下一个八字节整数，数据不足时返回 false
    bool integer(uint64_t& value) {
        if (bytes.size() - offset < 8)
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value |= uint64_t(bytes[offset++]) << (8 * i);
        return true;
    }
    // 读取长度前缀数据并确认其长度不超过剩余缓冲区
    bool data(std::vector<uint8_t>& value) {
        uint64_t size = 0;
        if (!integer(size) || size > bytes.size() - offset)
            return false;
        value.assign(bytes.begin() + offset, bytes.begin() + offset + static_cast<size_t>(size));
        offset += static_cast<size_t>(size);
        return true;
    }
    // 读取不含 NUL 的字符串字段，拒绝非法字节内容
    bool string(std::string& value) {
        std::vector<uint8_t> dataValue;
        if (!data(dataValue))
            return false;
        value.assign(dataValue.begin(), dataValue.end());
        return value.find('\0') == std::string::npos;
    }
};
// 根据备份盐值派生 AES-GCM 所需的 256 位密钥
bool derive(const std::string& password, const std::vector<uint8_t>& salt,
            std::vector<uint8_t>& key) {
    key.resize(32);
    return crypto::pbkdf2Sha256(password, salt, crypto::kDefaultPbkdf2Iterations, key);
}
} // namespace

// 序列化自定义备份条目，并附加校验摘要及可选 AES-GCM 认证加密
bool encode(const std::vector<Entry>& entries, int compressionLevel, const std::string& password,
            std::vector<uint8_t>& output, std::string& error) {
    if (entries.size() > maxEntries) {
        error = "备份条目过多";
        return false;
    }
    std::vector<uint8_t> body;
    put64(body, entries.size());
    for (const auto& entry : entries) {
        const auto& file = entry.file;
        if (file.type > 7 || !linux_file::validRelativePath(file.relativePath)) {
            error = "备份条目路径或类型无效: " + file.relativePath;
            return false;
        }
        if (!dbackup::backup_policy::archiveMetadataMatchesType(file.type, file.metadata.mode,
                                                                file.metadata.deviceMajor,
                                                                file.metadata.deviceMinor)) {
            error = "备份文件类型与元数据不一致: " + file.relativePath;
            return false;
        }
        if (file.type != 0 && !entry.data.empty()) {
            error = "非普通文件包含数据";
            return false;
        }
        put64(body, file.type);
        putString(body, file.relativePath);
        put64(body, file.metadata.mode);
        put64(body, file.metadata.uid);
        put64(body, file.metadata.gid);
        put64(body, static_cast<uint64_t>(file.metadata.mtimeNs));
        put64(body, file.metadata.deviceMajor);
        put64(body, file.metadata.deviceMinor);
        putString(body, file.owner);
        putString(body, file.linkTarget);
        put64(body, entry.data.size());
        std::vector<uint8_t> compressed;
        bool useCompression = false;
        if (!entry.data.empty() && entry.data.size() <= std::numeric_limits<uLong>::max()) {
            uLongf size = compressBound(static_cast<uLong>(entry.data.size()));
            compressed.resize(size);
            if (compress2(compressed.data(), &size, entry.data.data(),
                          static_cast<uLong>(entry.data.size()),
                          std::clamp(compressionLevel, 0, 9)) != Z_OK) {
                error = "压缩文件失败: " + file.relativePath;
                return false;
            }
            compressed.resize(size);
            useCompression = compressed.size() < entry.data.size();
        }
        put64(body, useCompression ? 1 : 0);
        const auto& payload = useCompression ? compressed : entry.data;
        putBytes(body, payload.data(), payload.size());
    }
    std::vector<uint8_t> digest;
    if (!crypto::sha256(body.data(), body.size(), digest)) {
        error = "无法计算备份校验值";
        return false;
    }
    body.insert(body.end(), digest.begin(), digest.end());
    output.assign(std::begin(magic), std::end(magic));
    output.push_back(password.empty() ? 0 : 1);
    if (!password.empty()) {
        std::vector<uint8_t> salt, nonce, tag, key, ciphertext;
        if (!crypto::randomBytes(salt, saltSize) || !crypto::randomBytes(nonce, nonceSize) ||
            !derive(password, salt, key) ||
            !crypto::aes256Gcm(true, body, key, nonce, tag, ciphertext)) {
            error = "备份加密失败";
            output.clear();
            return false;
        }
        output.insert(output.end(), salt.begin(), salt.end());
        output.insert(output.end(), nonce.begin(), nonce.end());
        output.insert(output.end(), tag.begin(), tag.end());
        output.insert(output.end(), ciphertext.begin(), ciphertext.end());
    } else
        output.insert(output.end(), body.begin(), body.end());
    return true;
}

// 校验并解析自定义备份，密码错误、摘要异常或路径不安全时停止解码
bool decode(const std::vector<uint8_t>& input, const std::string& password,
            std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    if (input.size() < sizeof(magic) + 1 ||
        !std::equal(std::begin(magic), std::end(magic), input.begin())) {
        error = "不是当前 Linux 版本支持的备份格式";
        return false;
    }
    size_t offset = sizeof(magic);
    const uint8_t encrypted = input[offset++];
    if (encrypted > 1) {
        error = "备份标志无效";
        return false;
    }
    std::vector<uint8_t> body;
    if (encrypted) {
        if (password.empty() || input.size() - offset < saltSize + nonceSize + tagSize) {
            error = "备份需要密码或数据不完整";
            return false;
        }
        std::vector<uint8_t> salt(input.begin() + offset, input.begin() + offset + saltSize);
        offset += saltSize;
        std::vector<uint8_t> nonce(input.begin() + offset, input.begin() + offset + nonceSize);
        offset += nonceSize;
        std::vector<uint8_t> tag(input.begin() + offset, input.begin() + offset + tagSize);
        offset += tagSize;
        std::vector<uint8_t> ciphertext(input.begin() + offset, input.end()), key;
        if (!derive(password, salt, key) ||
            !crypto::aes256Gcm(false, ciphertext, key, nonce, tag, body)) {
            error = "密码错误或备份已损坏";
            return false;
        }
    } else
        body.assign(input.begin() + offset, input.end());
    if (body.size() < digestSize) {
        error = "备份数据不完整";
        return false;
    }
    std::vector<uint8_t> actual;
    if (!crypto::sha256(body.data(), body.size() - digestSize, actual) ||
        !std::equal(actual.begin(), actual.end(), body.end() - digestSize)) {
        error = "备份校验失败";
        return false;
    }
    body.resize(body.size() - digestSize);
    Reader reader{body};
    uint64_t count = 0;
    if (!reader.integer(count) || count > maxEntries) {
        error = "备份条目数量无效";
        return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
        Entry entry;
        uint64_t type, mode, uid, gid, mtime, majorNumber, minorNumber, size, compressed;
        if (!reader.integer(type) || type > 7 || !reader.string(entry.file.relativePath) ||
            !linux_file::validRelativePath(entry.file.relativePath) || !reader.integer(mode) ||
            !reader.integer(uid) || !reader.integer(gid) || !reader.integer(mtime) ||
            !reader.integer(majorNumber) || !reader.integer(minorNumber) || mode > UINT32_MAX ||
            uid > UINT32_MAX || gid > UINT32_MAX || majorNumber > UINT32_MAX ||
            minorNumber > UINT32_MAX ||
            ((type != 5 && type != 6) && (majorNumber || minorNumber)) ||
            !reader.string(entry.file.owner) || !reader.string(entry.file.linkTarget) ||
            !reader.integer(size) || !reader.integer(compressed) || compressed > 1 ||
            !reader.data(entry.data)) {
            error = "备份条目无效";
            entries.clear();
            return false;
        }
        entry.compressed = compressed != 0;
        entry.file.type = static_cast<uint8_t>(type);
        entry.file.metadata.mode = static_cast<uint32_t>(mode);
        entry.file.metadata.uid = static_cast<uint32_t>(uid);
        entry.file.metadata.gid = static_cast<uint32_t>(gid);
        entry.file.metadata.mtimeNs = static_cast<int64_t>(mtime);
        entry.file.metadata.deviceMajor = static_cast<uint32_t>(majorNumber);
        entry.file.metadata.deviceMinor = static_cast<uint32_t>(minorNumber);
        entry.file.metadata.size = size;
        if (!dbackup::backup_policy::archiveMetadataMatchesType(
                static_cast<uint8_t>(type), static_cast<uint32_t>(mode),
                static_cast<uint32_t>(majorNumber), static_cast<uint32_t>(minorNumber))) {
            error = "备份文件类型与元数据不一致";
            entries.clear();
            return false;
        }
        if (type != 0 && (size || !entry.data.empty() || compressed)) {
            error = "非普通文件包含数据";
            entries.clear();
            return false;
        }
        if (compressed) {
            if (size > std::numeric_limits<uLongf>::max() || size > entry.data.max_size() ||
                entry.data.size() > std::numeric_limits<uLong>::max()) {
                error = "备份文件过大";
                entries.clear();
                return false;
            }
            std::vector<uint8_t> plain(static_cast<size_t>(size));
            uLongf decodedSize = static_cast<uLongf>(size);
            if (uncompress(plain.data(), &decodedSize, entry.data.data(),
                           static_cast<uLong>(entry.data.size())) != Z_OK ||
                decodedSize != size) {
                error = "备份解压失败";
                entries.clear();
                return false;
            }
            entry.data = std::move(plain);
        } else if (entry.data.size() != size) {
            error = "备份文件长度不匹配";
            entries.clear();
            return false;
        }
        entries.push_back(std::move(entry));
    }
    if (reader.offset != body.size()) {
        error = "备份包含多余数据";
        entries.clear();
        return false;
    }
    return true;
}
} // namespace dbackup::archive_format


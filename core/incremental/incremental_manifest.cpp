// 管理文件级增量备份清单
// 编码与解码版本清单，保护敏感内容，并校验版本依赖、文件状态与备份条目。
// 编码、解码和保护完整文件状态，校验版本依赖、删除记录及备份条目
#include "core/incremental/incremental_manifest.h"
#include "core/security/crypto.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <openssl/crypto.h>
#include <zlib.h>

namespace dbackup::incremental {
namespace {
constexpr size_t maxBytes = 64u * 1024u * 1024u;
constexpr uint32_t maxEntries = 1000000;
constexpr uint32_t maxString = 1024u * 1024u;
constexpr uint8_t magicV1[] = {'D', 'B', 'I', 'M', '0', '0', '0', '1'};
constexpr uint8_t magic[] = {'D', 'B', 'I', 'M', '0', '0', '0', '2'};
constexpr uint8_t protectedMagic[] = {'D', 'B', 'I', 'E', '0', '0', '0', '1'};

// 离开作用域时擦除敏感字节缓冲区。
struct WipeBytes {
    std::vector<uint8_t>& bytes;
    ~WipeBytes() { if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

// 检查单份清单的版本关系、文件路径及删除记录是否有效
bool validate(const Manifest& m, std::string& error) {
    if (!m.sequence || (m.full ? m.previous != 0 : (!m.previous || m.previous >= m.sequence)) ||
        m.files.size() > maxEntries || m.deleted.size() > maxEntries ||
        (m.full && !m.deleted.empty())) {
        error = "增量清单的版本或条目数量无效";
        return false;
    }
    Changes ignored;
    if (!compare({}, m.files, ignored, error))
        return false;
    std::vector<FileState> removed;
    removed.reserve(m.deleted.size());
    for (const auto& path : m.deleted)
        removed.push_back(FileState{path});
    if (!compare({}, removed, ignored, error))
        return false;
    std::set<std::string> currentPaths;
    for (const auto& file : m.files)
        currentPaths.insert(file.path);
    for (const auto& path : m.deleted) {
        if (currentPaths.count(path)) {
            error = "增量清单同时包含和删除路径: " + path;
            return false;
        }
    }
    for (const auto& f : m.files)
        if (f.path.size() > maxString || f.linkTarget.size() > maxString ||
            f.contentDigest.size() > maxString) {
            error = "增量清单字段过长";
            return false;
        }
    for (const auto& path : m.deleted)
        if (path.size() > maxString) {
            error = "增量清单路径过长";
            return false;
        }
    return true;
}

// 按指定字节数将整数写入清单数据
void put(std::vector<uint8_t>& out, uint64_t n, int count) {
    for (int i = 0; i < count; ++i)
        out.push_back(static_cast<uint8_t>(n >> (8 * i)));
}
// 写入字符串长度及内容，供清单编码使用
void putString(std::vector<uint8_t>& out, const std::string& s) {
    put(out, s.size(), 4);
    out.insert(out.end(), s.begin(), s.end());
}
// 维护清单读取偏移并在解码数字和字符串时检查边界。
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos = 0, end = 0;
    // 从清单读取指定长度的整数并检查边界
    bool number(uint64_t& value, int count) {
        if (pos > end || static_cast<size_t>(count) > end - pos)
            return false;
        value = 0;
        for (int i = 0; i < count; ++i)
            value |= uint64_t(data[pos++]) << (8 * i);
        return true;
    }
    // 读取带长度前缀的字符串并检查输入边界
    bool string(std::string& value) {
        uint64_t size;
        if (!number(size, 4) || size > maxString || size > end - pos)
            return false;
        value.assign(reinterpret_cast<const char*>(data.data() + pos), static_cast<size_t>(size));
        pos += static_cast<size_t>(size);
        return true;
    }
};

// 从清单数据流读取一个文件状态，并检查输入边界
bool readFileState(Reader& r, FileState& f) {
    uint64_t type, mode, uid, gid, time, size, major, minor;
    if (!r.string(f.path) || !r.number(type, 1) || !r.number(mode, 4) || !r.number(uid, 4) ||
        !r.number(gid, 4) || !r.number(time, 8) || !r.number(size, 8) ||
        !r.number(major, 4) || !r.number(minor, 4) || !r.string(f.linkTarget) ||
        !r.string(f.contentDigest))
        return false;
    f.type = static_cast<uint8_t>(type);
    f.mode = static_cast<uint32_t>(mode);
    f.uid = static_cast<uint32_t>(uid);
    f.gid = static_cast<uint32_t>(gid);
    // Preserve the two's-complement timestamp bit pattern without an out-of-range cast
    if (time <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        f.mtimeNs = static_cast<int64_t>(time);
    else
        f.mtimeNs = -1 - static_cast<int64_t>(~time);
    f.size = size;
    f.deviceMajor = static_cast<uint32_t>(major);
    f.deviceMinor = static_cast<uint32_t>(minor);
    return true;
}
} // namespace

// 校验并序列化清单，附加摘要以检测数据损坏
bool encodeManifest(const Manifest& m, std::vector<uint8_t>& bytes, std::string& error) {
    error.clear();
    if (!validate(m, error))
        return false;
    // Check aggregate size before allocating or writing any output
    uint64_t estimated = 8 + 8 + 8 + 8 + 1 + 4 + 4 + 4;
    for (const auto& f : m.files)
        estimated += 49ull + f.path.size() + f.linkTarget.size() + f.contentDigest.size();
    for (const auto& path : m.deleted)
        estimated += 4ull + path.size();
    if (estimated > maxBytes) {
        error = "增量清单超过大小限制";
        return false;
    }
    std::vector<uint8_t> out(std::begin(magic), std::end(magic));
    put(out, m.sequence, 8);
    put(out, m.previous, 8);
    put(out, static_cast<uint64_t>(m.createdAtMs), 8);
    put(out, m.full ? 1 : 0, 1);
    put(out, m.files.size(), 4);
    put(out, m.deleted.size(), 4);
    for (const auto& f : m.files) {
        putString(out, f.path);
        put(out, f.type, 1);
        put(out, f.mode, 4);
        put(out, f.uid, 4);
        put(out, f.gid, 4);
        put(out, static_cast<uint64_t>(f.mtimeNs), 8);
        put(out, f.size, 8);
        put(out, f.deviceMajor, 4);
        put(out, f.deviceMinor, 4);
        putString(out, f.linkTarget);
        putString(out, f.contentDigest);
    }
    for (const auto& path : m.deleted)
        putString(out, path);
    const auto checksum = crc32(0, out.data(), static_cast<uInt>(out.size()));
    put(out, checksum, 4);
    bytes = std::move(out);
    return true;
}

// 检查数据格式及摘要，解析校验成功后更新输出
bool decodeManifest(const std::vector<uint8_t>& bytes, Manifest& manifest, std::string& error) {
    error.clear();
    const bool v1 = bytes.size() >= 8 && std::equal(std::begin(magicV1), std::end(magicV1), bytes.begin());
    const bool v2 = bytes.size() >= 8 && std::equal(std::begin(magic), std::end(magic), bytes.begin());
    if (bytes.size() < 37 || bytes.size() > maxBytes || (!v1 && !v2)) {
        error = "增量清单格式或大小无效";
        return false;
    }
    Reader checksumReader{bytes, bytes.size() - 4, bytes.size()};
    uint64_t expected;
    if (!checksumReader.number(expected, 4) ||
        crc32(0, bytes.data(), static_cast<uInt>(bytes.size() - 4)) != expected) {
        error = "增量清单校验失败";
        return false;
    }
    Reader r{bytes, sizeof(magic), bytes.size() - 4};
    Manifest parsed;
    uint64_t full, count, deleted, timestamp = 0;
    if (!r.number(parsed.sequence, 8) || !r.number(parsed.previous, 8) ||
        (v2 && !r.number(timestamp, 8)) || !r.number(full, 1) ||
        full > 1 || !r.number(count, 4) || !r.number(deleted, 4) || count > maxEntries ||
        deleted > maxEntries || count > (r.end - r.pos) / 49 || deleted > (r.end - r.pos) / 4) {
        error = "增量清单头无效";
        return false;
    }
    parsed.createdAtMs = timestamp <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? static_cast<int64_t>(timestamp) : -1 - static_cast<int64_t>(~timestamp);
    parsed.full = full == 1;
    parsed.files.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        FileState f;
        if (!readFileState(r, f)) {
            error = "增量清单文件条目无效";
            return false;
        }
        parsed.files.push_back(std::move(f));
    }
    parsed.deleted.reserve(static_cast<size_t>(deleted));
    for (uint64_t i = 0; i < deleted; ++i) {
        std::string path;
        if (!r.string(path)) {
            error = "增量清单删除条目无效";
            return false;
        }
        parsed.deleted.push_back(std::move(path));
    }
    if (r.pos != r.end || !validate(parsed, error)) {
        if (error.empty())
            error = "增量清单包含多余数据";
        return false;
    }
    manifest = std::move(parsed);
    return true;
}

// 编码清单并使用密码派生密钥进行认证加密
bool encodeProtectedManifest(const Manifest& manifest, const std::string& password,
                             std::vector<uint8_t>& bytes, std::string& error) {
    error.clear();
    if (password.empty()) {
        error = "加密清单需要密码";
        return false;
    }
    std::vector<uint8_t> plain, salt, nonce, tag, encrypted, key(32);
    WipeBytes wipePlain{plain}, wipeKey{key};
    if (!encodeManifest(manifest, plain, error))
        return false;
    if (!crypto::randomBytes(salt, 16) ||
        !crypto::pbkdf2Sha256(password, salt, crypto::kDefaultPbkdf2Iterations, key)) {
        error = "无法生成清单加密密钥";
        return false;
    }
    std::vector<uint8_t> header(std::begin(protectedMagic), std::end(protectedMagic));
    header.insert(header.end(), salt.begin(), salt.end());
    if (!crypto::aes256Gcm(true, plain, key, nonce, tag, header, encrypted)) {
        error = "清单加密失败";
        return false;
    }
    std::vector<uint8_t> result = std::move(header);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), tag.begin(), tag.end());
    result.insert(result.end(), encrypted.begin(), encrypted.end());
    bytes = std::move(result);
    return true;
}

// 验证认证标签并解密，再解析并校验清单
bool decodeProtectedManifest(const std::vector<uint8_t>& bytes, const std::string& password,
                             Manifest& manifest, std::string& error) {
    error.clear();
    constexpr size_t headerSize = 8 + 16, prefixSize = headerSize + 12 + 16;
    if (password.empty() || bytes.size() < prefixSize + 37 ||
        bytes.size() > prefixSize + maxBytes ||
        !std::equal(std::begin(protectedMagic), std::end(protectedMagic), bytes.begin())) {
        error = "加密清单格式无效或缺少密码";
        return false;
    }
    std::vector<uint8_t> header(bytes.begin(), bytes.begin() + headerSize);
    std::vector<uint8_t> salt(bytes.begin() + 8, bytes.begin() + headerSize);
    std::vector<uint8_t> nonce(bytes.begin() + headerSize, bytes.begin() + headerSize + 12);
    std::vector<uint8_t> tag(bytes.begin() + headerSize + 12, bytes.begin() + prefixSize);
    std::vector<uint8_t> encrypted(bytes.begin() + prefixSize, bytes.end());
    std::vector<uint8_t> key(32), plain;
    WipeBytes wipeKey{key}, wipePlain{plain};
    if (!crypto::pbkdf2Sha256(password, salt, crypto::kDefaultPbkdf2Iterations, key) ||
        !crypto::aes256Gcm(false, encrypted, key, nonce, tag, header, plain)) {
        error = "清单密码错误或数据已损坏";
        return false;
    }
    return decodeManifest(plain, manifest, error);
}

// 逐个检查版本顺序、依赖关系及删除记录
bool validateChain(const std::vector<Manifest>& chain, std::string& error) {
    error.clear();
    if (chain.empty()) {
        error = "备份链为空";
        return false;
    }
    const Manifest* previous = nullptr;
    for (const auto& item : chain) {
        if (!validate(item, error))
            return false;
        if (!previous && !item.full) {
            error = "备份链缺少起始完整备份";
            return false;
        }
        if (previous && item.sequence <= previous->sequence) {
            error = "备份链版本顺序无效";
            return false;
        }
        if (!item.full) {
            if (!previous || item.previous != previous->sequence) {
                error = "备份链缺少前序版本";
                return false;
            }
            Changes changes;
            if (!compare(previous->files, item.files, changes, error))
                return false;
            auto recorded = item.deleted;
            std::sort(recorded.begin(), recorded.end());
            if (recorded != changes.deleted) {
                error = "备份链删除记录与文件状态不一致";
                return false;
            }
        }
        previous = &item;
    }
    return true;
}

// 计算不破坏保留版本依赖的可删除前缀长度
bool removablePrefix(const std::vector<Manifest>& chain, size_t keepLatest, size_t& count,
                     std::string& error) {
    if (!keepLatest) {
        error = "保留数量必须大于零";
        return false;
    }
    if (!validateChain(chain, error))
        return false;
    const size_t firstKept = chain.size() > keepLatest ? chain.size() - keepLatest : 0;
    size_t checkpoint = firstKept;
    while (checkpoint > 0 && !chain[checkpoint].full)
        --checkpoint;
    count = checkpoint;
    return true;
}

// 对照版本清单检查备份条目的路径、类型和链接
bool validateArchiveEntries(const Manifest& current, const Manifest* previous,
                            const std::vector<ArchiveEntry>& entries, std::string& error) {
    error.clear();
    if (!validate(current, error))
        return false;
    std::vector<std::string> expected;
    if (current.full) {
        for (const auto& state : current.files)
            expected.push_back(state.path);
    } else {
        if (!previous || !validate(*previous, error)) {
            if (error.empty())
                error = "缺少前序版本清单";
            return false;
        }
        if (current.previous != previous->sequence) {
            error = "备份与前序版本不匹配";
            return false;
        }
        Changes changes;
        if (!compare(previous->files, current.files, changes, error))
            return false;
        auto deleted = current.deleted;
        std::sort(deleted.begin(), deleted.end());
        if (deleted != changes.deleted) {
            error = "增量清单删除记录与文件状态不一致";
            return false;
        }
        expected = std::move(changes.changed);
    }
    std::map<std::string, const FileState*> states;
    for (const auto& state : current.files)
        states.emplace(state.path, &state);
    std::set<std::string> wanted(expected.begin(), expected.end());
    std::set<std::string> found;
    for (const auto& entry : entries) {
        const auto state = states.find(entry.relativePath);
        if (!wanted.count(entry.relativePath) || !found.insert(entry.relativePath).second ||
            state == states.end()) {
            error = "备份包含多余或重复条目: " + entry.relativePath;
            return false;
        }
        const FileState& file = *state->second;
        const bool independentHardlink = file.type == 3 && entry.type == 0;
        if ((!independentHardlink && entry.type != file.type) ||
            ((entry.type == 0) && entry.originalSize != file.size) ||
            ((entry.type == 2 || entry.type == 3) && entry.linkTarget != file.linkTarget)) {
            error = "备份条目与清单不一致: " + entry.relativePath;
            return false;
        }
    }
    if (found != wanted) {
        error = "备份缺少清单所需的条目";
        return false;
    }
    return true;
}
} // namespace dbackup::incremental

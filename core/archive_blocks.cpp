#include "core/archive_blocks.h"
#include "core/compression.h"
#include "core/crypto.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <unistd.h>
#include <vector>

namespace dbackup::archive_blocks {
namespace {
constexpr size_t blockSize = 4u * 1024u * 1024u;
constexpr size_t maxEncoded = 32u * 1024u * 1024u;
// Four-byte format identifier followed by a little-endian version number.
constexpr uint8_t magic[] = {'D', 'B', 'P', 'K', 2, 0, 0, 0};
constexpr char previousMagic[] = "DBPK0002";
using Bytes = std::vector<uint8_t>;
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void put(Bytes& data, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        data.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
uint64_t get(const uint8_t* data, unsigned size) {
    uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i)
        result |= uint64_t(data[i]) << (8 * i);
    return result;
}
bool writeAll(int fd, const uint8_t* data, size_t size, std::string& error) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            error = "写入归档失败: " + std::string(std::strerror(errno));
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}
bool readExact(int fd, uint8_t* data, size_t size, std::string& error) {
    while (size) {
        ssize_t n = read(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            error = n == 0 ? "归档数据截断" : "读取归档失败: " + std::string(std::strerror(errno));
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}
bool keyFor(const std::string& password, const Bytes& salt, Bytes& key) {
    key.resize(32);
    return crypto::pbkdf2Sha256(password, salt, crypto::kDefaultPbkdf2Iterations, key);
}
Bytes nonceFor(const Bytes& header, uint32_t sequence) {
    Bytes nonce(header.begin() + 28, header.begin() + 36);
    put(nonce, sequence, 4);
    return nonce;
}
bool crypt(bool encrypt, const Bytes& header, const Bytes& record, const Bytes& input,
           const Bytes& key, uint32_t sequence, Bytes& tag, Bytes& output) {
    Bytes nonce = nonceFor(header, sequence);
    Bytes aad = header;
    aad.insert(aad.end(), record.begin(), record.end());
    return crypto::aes256Gcm(encrypt, input, key, nonce, tag, aad, output);
}
bool cancelled(const OperationContext& context, std::string& error) {
    if (!context.isCancelled())
        return false;
    error = "操作已取消";
    return true;
}
} // namespace

bool encode(int packedFd, int archiveFd, const PackOptions& options,
            const OperationContext& context, std::string& error) {
    if (uint8_t(options.format) > 1 || uint8_t(options.compression) > 3) {
        error = "归档算法无效";
        return false;
    }
    const bool encrypted = !options.password.empty();
    Bytes salt(16), prefix(8), key;
    if (encrypted && (!crypto::randomBytes(salt, 16) || !crypto::randomBytes(prefix, 8) ||
                      !keyFor(options.password, salt, key))) {
        error = "生成归档密钥失败";
        return false;
    }
    Bytes header(magic, magic + 8);
    header.push_back(uint8_t(options.format));
    header.push_back(uint8_t(options.compression));
    header.push_back(encrypted ? 1 : 0);
    header.push_back(0);
    header.insert(header.end(), salt.begin(), salt.end());
    header.insert(header.end(), prefix.begin(), prefix.end());
    put(header, blockSize, 4);
    if (!writeAll(archiveFd, header.data(), header.size(), error))
        return false;
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(digest.get(), header.data(), header.size()) != 1) {
        error = "无法初始化归档摘要";
        return false;
    }
    uint64_t total = 0;
    uint32_t sequence = 0;
    Bytes plain(blockSize);
    for (;;) {
        if (cancelled(context, error))
            return false;
        size_t used = 0;
        while (used < blockSize) {
            ssize_t n = read(packedFd, plain.data() + used, blockSize - used);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                error = "读取打包数据失败: " + std::string(std::strerror(errno));
                return false;
            }
            if (!n)
                break;
            used += static_cast<size_t>(n);
        }
        if (!used)
            break;
        if (sequence == UINT32_MAX || total > UINT64_MAX - used) {
            error = "归档数据超出格式限制";
            return false;
        }
        plain.resize(used);
        Bytes encoded;
        if (EVP_DigestUpdate(digest.get(), plain.data(), used) != 1 ||
            !compression::encode(options.compression, plain, encoded, error) ||
            encoded.size() > maxEncoded) {
            if (error.empty()) error = "归档数据块编码失败或超限";
            return false;
        }
        Bytes record{1};
        put(record, sequence, 4);
        put(record, used, 4);
        put(record, encoded.size(), 4);
        Bytes tag, payload;
        if (encrypted) {
            if (!crypt(true, header, record, encoded, key, sequence, tag, payload)) {
                error = "归档数据块加密失败";
                return false;
            }
        } else payload = std::move(encoded);
        if (!writeAll(archiveFd, record.data(), record.size(), error) ||
            !writeAll(archiveFd, payload.data(), payload.size(), error) ||
            (encrypted && !writeAll(archiveFd, tag.data(), tag.size(), error)))
            return false;
        total += used;
        ++sequence;
        plain.resize(blockSize);
    }
    Bytes record{2};
    put(record, sequence, 4);
    put(record, total, 8);
    Bytes hash(32);
    unsigned len = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &len) != 1 || len != 32) {
        error = "计算归档摘要失败";
        return false;
    }
    record.insert(record.end(), hash.begin(), hash.end());
    Bytes tag, empty;
    if (encrypted && !crypt(true, header, record, {}, key, sequence, tag, empty)) {
        error = "归档结束记录认证失败";
        return false;
    }
    return writeAll(archiveFd, record.data(), record.size(), error) &&
           (!encrypted || writeAll(archiveFd, tag.data(), tag.size(), error));
}

bool decode(int archiveFd, int packedFd, const std::string& password, PackFormat& format,
            const OperationContext& context, std::string& error) {
    Bytes header(40);
    if (!readExact(archiveFd, header.data(), header.size(), error)) return false;
    if ((std::memcmp(header.data(), magic, 8) &&
         std::memcmp(header.data(), previousMagic, 8)) || header[8] > 1 || header[9] > 3 ||
        header[10] > 1 || header[11] || get(header.data() + 36, 4) != blockSize) {
        error = "归档格式或参数无效";
        return false;
    }
    if (!header[10] && std::any_of(header.begin() + 12, header.begin() + 36,
                                   [](uint8_t b) { return b != 0; })) {
        error = "归档头校验失败";
        return false;
    }
    format = PackFormat(header[8]);
    const auto method = Compression(header[9]);
    const bool encrypted = header[10] != 0;
    Bytes key, salt(header.begin() + 12, header.begin() + 28);
    if (encrypted && (password.empty() || !keyFor(password, salt, key))) {
        error = "归档需要密码或密钥派生失败";
        return false;
    }
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(digest.get(), header.data(), header.size()) != 1) {
        error = "无法初始化归档摘要";
        return false;
    }
    uint64_t total = 0;
    uint32_t sequence = 0;
    for (;;) {
        if (cancelled(context, error)) return false;
        uint8_t kind;
        if (!readExact(archiveFd, &kind, 1, error)) return false;
        if (kind == 2) {
            Bytes record(45);
            record[0] = kind;
            if (!readExact(archiveFd, record.data() + 1, 44, error)) return false;
            if (get(record.data() + 1, 4) != sequence || get(record.data() + 5, 8) != total) {
                error = "归档结束记录不匹配";
                return false;
            }
            if (encrypted) {
                Bytes tag(16), output;
                if (!readExact(archiveFd, tag.data(), tag.size(), error) ||
                    !crypt(false, header, record, {}, key, sequence, tag, output)) {
                    error = "密码错误或归档结束记录已损坏";
                    return false;
                }
            }
            Bytes actual(32);
            unsigned len = 0;
            if (EVP_DigestFinal_ex(digest.get(), actual.data(), &len) != 1 || len != 32 ||
                !std::equal(actual.begin(), actual.end(), record.begin() + 13)) {
                error = "归档摘要校验失败";
                return false;
            }
            uint8_t extra;
            ssize_t n;
            do { n = read(archiveFd, &extra, 1); } while (n < 0 && errno == EINTR);
            if (n != 0) {
                error = "归档结束记录后包含多余数据或读取失败";
                return false;
            }
            return true;
        }
        if (kind != 1 || sequence == UINT32_MAX) {
            error = "归档数据块类型或数量无效";
            return false;
        }
        Bytes record(13);
        record[0] = kind;
        if (!readExact(archiveFd, record.data() + 1, 12, error)) return false;
        const uint64_t index = get(record.data() + 1, 4);
        const uint64_t raw = get(record.data() + 5, 4), encodedSize = get(record.data() + 9, 4);
        if (index != sequence || !raw || raw > blockSize || encodedSize > maxEncoded ||
            total > UINT64_MAX - raw) {
            error = "归档数据块序号或长度无效";
            return false;
        }
        Bytes payload(encodedSize), encoded, tag;
        if (!readExact(archiveFd, payload.data(), payload.size(), error)) return false;
        if (encrypted) {
            tag.resize(16);
            if (!readExact(archiveFd, tag.data(), tag.size(), error) ||
                !crypt(false, header, record, payload, key, sequence, tag, encoded)) {
                error = "密码错误或归档数据块已损坏";
                return false;
            }
        } else encoded = std::move(payload);
        Bytes plain;
        if (!compression::decode(method, encoded, raw, plain, error) || plain.size() != raw ||
            EVP_DigestUpdate(digest.get(), plain.data(), plain.size()) != 1 ||
            !writeAll(packedFd, plain.data(), plain.size(), error)) {
            if (error.empty()) error = "归档数据块解码失败";
            return false;
        }
        total += raw;
        ++sequence;
    }
}
} // namespace dbackup::archive_blocks

#include "core/archive_internal.h"
#include "core/archive_tar.h"
#include "core/compression.h"
#include "core/crypto.h"
#include <algorithm>
#include <cstring>

namespace dbackup::archive_format {
namespace {
constexpr char magic[] = "DBPK0001";
// 将整数按封装格式的小端顺序写入字节流。
void put64(std::vector<uint8_t>& o, uint64_t n) {
    for (int i = 0; i < 8; i++)
        o.push_back(n >> (8 * i));
}
// 从小端字节流读取固定八字节无符号整数。
uint64_t get64(const uint8_t* p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; i++)
        n |= uint64_t(p[i]) << (8 * i);
    return n;
}
// 从密码和随机盐派生封装 AES-GCM 所需的密钥。
bool keyFor(const std::string& p, const std::vector<uint8_t>& s, std::vector<uint8_t>& k) {
    k.resize(32);
    return crypto::pbkdf2Sha256(p, s, crypto::kDefaultPbkdf2Iterations, k);
}
} // namespace
bool encodeCurrent(const std::vector<Entry>& entries, const PackOptions& options,
                   std::vector<uint8_t>& out, std::string& error) {
    std::vector<uint8_t> packed;
    if (options.format == PackFormat::Tar) {
        if (!archive_tar::encode(entries, packed, error))
            return false;
    } else if (options.format == PackFormat::Custom) {
        if (!encode(entries, 0, "", packed, error))
            return false;
    } else {
        error = "未知打包格式";
        return false;
    }
    Compression method = options.compression;
    if (method == Compression::Deflate && options.compressionLevel == 0)
        method = Compression::None;
    if (options.format == PackFormat::Tar && method == Compression::None &&
        options.password.empty()) {
        out = std::move(packed);
        return true;
    }
    if (packed.size() > size_t(2) * 1024 * 1024 * 1024) {
        error = "归档数据超过 2 GiB 解压限制";
        return false;
    }
    std::vector<uint8_t> compressed;
    if (!compression::encode(method, packed, compressed, error))
        return false;
    std::vector<uint8_t> body{uint8_t(options.format), uint8_t(method)};
    put64(body, packed.size());
    body.insert(body.end(), compressed.begin(), compressed.end());
    std::vector<uint8_t> digest;
    if (!crypto::sha256(body.data(), body.size(), digest)) {
        error = "无法计算校验值";
        return false;
    }
    body.insert(body.end(), digest.begin(), digest.end());
    out.assign(magic, magic + 8);
    out.push_back(options.password.empty() ? 0 : 1);
    if (options.password.empty()) {
        out.insert(out.end(), body.begin(), body.end());
        return true;
    }
    std::vector<uint8_t> salt, nonce, tag, key, cipher;
    if (!crypto::randomBytes(salt, 16) || !crypto::randomBytes(nonce, 12) ||
        !keyFor(options.password, salt, key) ||
        !crypto::aes256Gcm(true, body, key, nonce, tag, cipher)) {
        error = "加密失败";
        out.clear();
        return false;
    }
    for (const auto* part : {&salt, &nonce, &tag, &cipher})
        out.insert(out.end(), part->begin(), part->end());
    return true;
}
bool decodeCurrent(const std::vector<uint8_t>& in, const std::string& password,
                   std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    if (in.size() < 8 || std::memcmp(in.data(), magic, 8) != 0)
        return archive_tar::decode(in, entries, error);
    if (in.size() < 9 || in[8] > 1) {
        error = "归档标志无效";
        return false;
    }
    size_t p = 9;
    std::vector<uint8_t> body;
    if (in[8]) {
        if (password.empty() || in.size() - p < 44) {
            error = "归档需要密码或数据不完整";
            return false;
        }
        std::vector<uint8_t> salt(in.begin() + p, in.begin() + p + 16);
        p += 16;
        std::vector<uint8_t> nonce(in.begin() + p, in.begin() + p + 12);
        p += 12;
        std::vector<uint8_t> tag(in.begin() + p, in.begin() + p + 16);
        p += 16;
        std::vector<uint8_t> cipher(in.begin() + p, in.end()), key;
        if (!keyFor(password, salt, key) ||
            !crypto::aes256Gcm(false, cipher, key, nonce, tag, body)) {
            error = "密码错误或归档已损坏";
            return false;
        }
    } else
        body.assign(in.begin() + p, in.end());
    if (body.size() < 42) {
        error = "归档数据不完整";
        return false;
    }
    std::vector<uint8_t> digest;
    if (!crypto::sha256(body.data(), body.size() - 32, digest) ||
        !std::equal(digest.begin(), digest.end(), body.end() - 32)) {
        error = "归档校验失败";
        return false;
    }
    body.resize(body.size() - 32);
    if (body[0] > 1 || body[1] > 3) {
        error = "归档算法无效";
        return false;
    }
    uint64_t n = get64(body.data() + 2);
    if (n > uint64_t(2) * 1024 * 1024 * 1024) {
        error = "归档解压大小超过限制";
        return false;
    }
    std::vector<uint8_t> compressed(body.begin() + 10, body.end()), packed;
    if (!compression::decode(Compression(body[1]), compressed, size_t(n), packed, error))
        return false;
    bool ok = body[0] == uint8_t(PackFormat::Tar) ? archive_tar::decode(packed, entries, error)
                                                  : decode(packed, "", entries, error);
    if (ok && body[1] != uint8_t(Compression::None))
        for (auto& entry : entries)
            if (entry.file.type == 0)
                entry.compressed = true;
    return ok;
}
} // namespace dbackup::archive_format

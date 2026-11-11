#include "crypto.h"
#include <algorithm>
#include <climits>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace dbackup::crypto {
// 从 OpenSSL 安全随机源填充指定长度的随机缓冲区。
bool randomBytes(std::vector<uint8_t>& out, size_t size) {
    if (size > INT_MAX)
        return false;
    out.resize(size);
    if (size && RAND_bytes(out.data(), static_cast<int>(size)) != 1) {
        out.clear();
        return false;
    }
    return true;
}
// 计算输入数据的 SHA-256 摘要，失败时清空输出并返回 false。
bool sha256(const uint8_t* input, size_t size, std::vector<uint8_t>& digest) {
    if (size && !input)
        return false;
    digest.resize(32);
    unsigned int length = 0;
    if (EVP_Digest(input, size, digest.data(), &length, EVP_sha256(), nullptr) != 1) {
        digest.clear();
        return false;
    }
    digest.resize(length);
    return true;
}
bool pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt,
                  uint64_t iterations, std::vector<uint8_t>& key) {
    if (password.size() > INT_MAX || salt.size() > INT_MAX || key.size() > INT_MAX || !iterations ||
        iterations > INT_MAX)
        return false;
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(key.size()), key.data()) != 1) {
        key.clear();
        return false;
    }
    return true;
}
bool aes256Gcm(bool encrypt, const std::vector<uint8_t>& input, const std::vector<uint8_t>& key,
               std::vector<uint8_t>& nonce, std::vector<uint8_t>& tag,
               std::vector<uint8_t>& output) {
    return aes256Gcm(encrypt, input, key, nonce, tag, {}, output);
}
bool aes256Gcm(bool encrypt, const std::vector<uint8_t>& input, const std::vector<uint8_t>& key,
               std::vector<uint8_t>& nonce, std::vector<uint8_t>& tag,
               const std::vector<uint8_t>& associatedData, std::vector<uint8_t>& output) {
    if (key.size() != 32 || input.size() > output.max_size() - 16)
        return false;
    if (encrypt && nonce.empty() && !randomBytes(nonce, 12))
        return false;
    if (nonce.size() != 12)
        return false;
    if (encrypt)
        tag.resize(16);
    if (tag.size() != 16)
        return false;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    if (!ctx)
        return false;
    output.resize(input.size() + 16);
    int length = 0;
    size_t total = 0;
    bool ok = EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce.data(),
                                encrypt ? 1 : 0) == 1;
    if (!encrypt)
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1;
    constexpr size_t chunkSize = 1024 * 1024;
    for (size_t offset = 0; ok && offset < associatedData.size();) {
        const size_t count = std::min(chunkSize, associatedData.size() - offset);
        ok = EVP_CipherUpdate(ctx.get(), nullptr, &length, associatedData.data() + offset,
                              static_cast<int>(count)) == 1;
        offset += count;
    }
    for (size_t offset = 0; ok && offset < input.size();) {
        const size_t count = std::min(chunkSize, input.size() - offset);
        ok = EVP_CipherUpdate(ctx.get(), output.data() + total, &length, input.data() + offset,
                              static_cast<int>(count)) == 1;
        if (ok) {
            total += static_cast<size_t>(length);
            offset += count;
        }
    }
    ok = ok && EVP_CipherFinal_ex(ctx.get(), output.data() + total, &length) == 1;
    if (ok)
        total += static_cast<size_t>(length);
    if (encrypt)
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    if (!ok) {
        output.clear();
        return false;
    }
    output.resize(total);
    return true;
}
} // namespace dbackup::crypto

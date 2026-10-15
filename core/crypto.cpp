// Copyright 2026 BackupTool Team. All rights reserved.

#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <limits>

namespace dbackup::crypto {
namespace {

constexpr size_t kKeySize = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;

PUCHAR inputPointer(const std::vector<uint8_t> &input, uint8_t &emptyByte) {
    return input.empty() ? &emptyByte : const_cast<PUCHAR>(input.data());
}

}  // namespace

bool randomBytes(std::vector<uint8_t> &output, size_t size) {
    if (size > std::numeric_limits<ULONG>::max()) return false;
    output.resize(size);
    if (size == 0) return true;
    return BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool sha256(const uint8_t *input, size_t size, std::vector<uint8_t> &digest) {
    if (size > std::numeric_limits<ULONG>::max() || (size != 0 && input == nullptr)) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;

    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                &resultSize, 0) >= 0;
    std::vector<uint8_t> object(objectSize);
    digest.assign(32, 0);
    uint8_t emptyByte = 0;
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    ok = ok && BCryptHashData(hash, size == 0 ? &emptyByte : const_cast<PUCHAR>(input),
                              static_cast<ULONG>(size), 0) >= 0;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) digest.clear();
    return ok;
}

bool pbkdf2Sha256(const std::string &password, const std::vector<uint8_t> &salt,
                  uint64_t iterations, std::vector<uint8_t> &key) {
    if (password.size() > std::numeric_limits<ULONG>::max() ||
        salt.size() > std::numeric_limits<ULONG>::max() ||
        key.size() > std::numeric_limits<ULONG>::max() || iterations == 0) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    uint8_t emptyByte = 0;
    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        password.empty() ? &emptyByte : reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
        static_cast<ULONG>(password.size()),
        salt.empty() ? &emptyByte : const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()), iterations,
        key.data(), static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) key.clear();
    return status >= 0;
}

bool aes256Gcm(bool encrypt, const std::vector<uint8_t> &input,
               const std::vector<uint8_t> &key, std::vector<uint8_t> &nonce,
               std::vector<uint8_t> &tag, std::vector<uint8_t> &output) {
    if (input.size() > std::numeric_limits<ULONG>::max() || key.size() != kKeySize) return false;
    if (encrypt && nonce.empty() && !randomBytes(nonce, kNonceSize)) return false;
    if (nonce.size() != kNonceSize) return false;
    if (encrypt) tag.assign(kTagSize, 0);
    if (tag.size() != kTagSize) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                                sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0;
    ok = ok && BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                 reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                 &resultSize, 0) >= 0;
    std::vector<uint8_t> object(objectSize);
    ok = ok && BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(), objectSize,
                                          const_cast<PUCHAR>(key.data()),
                                          static_cast<ULONG>(key.size()), 0) >= 0;
    if (!ok) {
        if (keyHandle) BCryptDestroyKey(keyHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = nonce.data();
    auth.cbNonce = static_cast<ULONG>(nonce.size());
    auth.pbTag = tag.data();
    auth.cbTag = static_cast<ULONG>(tag.size());

    DWORD required = 0;
    DWORD actual = 0;
    uint8_t emptyByte = 0;
    PUCHAR source = inputPointer(input, emptyByte);
    NTSTATUS status = encrypt
        ? BCryptEncrypt(keyHandle, source, static_cast<ULONG>(input.size()), &auth,
                        nullptr, 0, nullptr, 0, &required, 0)
        : BCryptDecrypt(keyHandle, source, static_cast<ULONG>(input.size()), &auth,
                        nullptr, 0, nullptr, 0, &required, 0);
    if (status >= 0) {
        output.resize(required);
        PUCHAR destination = output.empty() ? &emptyByte : output.data();
        status = encrypt
            ? BCryptEncrypt(keyHandle, source, static_cast<ULONG>(input.size()), &auth,
                            nullptr, 0, destination, required, &actual, 0)
            : BCryptDecrypt(keyHandle, source, static_cast<ULONG>(input.size()), &auth,
                            nullptr, 0, destination, required, &actual, 0);
        if (status >= 0) output.resize(actual);
    }

    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) output.clear();
    return status >= 0;
}

}  // namespace dbackup::crypto

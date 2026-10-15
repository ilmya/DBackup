// Copyright 2026 BackupTool Team. All rights reserved.

#ifndef DBACKUP_CORE_CRYPTO_H_
#define DBACKUP_CORE_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::crypto {

inline constexpr uint64_t kDefaultPbkdf2Iterations = 100000;

bool randomBytes(std::vector<uint8_t> &output, size_t size);
bool sha256(const uint8_t *input, size_t size, std::vector<uint8_t> &digest);
bool pbkdf2Sha256(const std::string &password, const std::vector<uint8_t> &salt,
                  uint64_t iterations, std::vector<uint8_t> &key);

// Uses a 256-bit key, a 96-bit nonce and a 128-bit authentication tag.
// Encryption generates a nonce when the supplied nonce is empty.
bool aes256Gcm(bool encrypt, const std::vector<uint8_t> &input,
               const std::vector<uint8_t> &key, std::vector<uint8_t> &nonce,
               std::vector<uint8_t> &tag, std::vector<uint8_t> &output);

}  // namespace dbackup::crypto

#endif  // DBACKUP_CORE_CRYPTO_H_

// 封装备份使用的密码学基础能力
// 声明 SHA-256、PBKDF2 和 AES-256-GCM 等基础接口及其字节容器约定。
// 提供安全随机数、SHA-256、PBKDF2 密钥派生和 AES-256-GCM 认证加解密
// Copyright 2026 BackupTool Team. All rights reserved

#ifndef DBACKUP_CORE_CRYPTO_H_
#define DBACKUP_CORE_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::crypto {

inline constexpr uint64_t kDefaultPbkdf2Iterations = 100000;

// 从操作系统安全随机源生成指定长度的随机字节
bool randomBytes(std::vector<uint8_t>& output, size_t size);
// 计算 SHA-256 摘要，输出长度固定为 32 字节
bool sha256(const uint8_t* input, size_t size, std::vector<uint8_t>& digest);
// 使用 PBKDF2-HMAC-SHA256 从密码和盐派生密钥
bool pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt,
                  uint64_t iterations, std::vector<uint8_t>& key);

// 使用 AES-256-GCM 加解密；加密时可自动生成随机 nonce，解密时校验认证标签
bool aes256Gcm(bool encrypt, const std::vector<uint8_t>& input, const std::vector<uint8_t>& key,
               std::vector<uint8_t>& nonce, std::vector<uint8_t>& tag,
               std::vector<uint8_t>& output);
// 附加认证数据参与 GCM 认证，但不会加密或写入输出
bool aes256Gcm(bool encrypt, const std::vector<uint8_t>& input, const std::vector<uint8_t>& key,
               std::vector<uint8_t>& nonce, std::vector<uint8_t>& tag,
               const std::vector<uint8_t>& associatedData, std::vector<uint8_t>& output);

} // namespace dbackup::crypto

#endif // DBACKUP_CORE_CRYPTO_H_

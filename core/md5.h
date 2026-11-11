#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace dbackup::crypto {
// 计算 MD5 展示摘要；此结果不用于认证或安全校验。
std::string md5(const uint8_t* data, size_t size);
// 分块读取文件并生成 MD5 十六进制摘要，读取失败时返回原因。
bool md5File(const std::string& path, std::string& digest, std::string& error);
} // namespace dbackup::crypto

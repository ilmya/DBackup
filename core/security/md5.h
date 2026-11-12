// 提供内存数据和文件的 MD5 摘要计算
// 声明内存数据与文件的 MD5 摘要接口，不将 MD5 作为认证机制。
// 以分块方式处理输入并输出十六进制结果，供需要 MD5 的校验场景使用
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace dbackup::crypto {
// 计算 MD5 展示摘要；此结果不用于认证或安全校验
std::string md5(const uint8_t* data, size_t size);
// 分块读取文件并生成 MD5 十六进制摘要，读取失败时返回原因
bool md5File(const std::string& path, std::string& digest, std::string& error);
} // namespace dbackup::crypto

// 提供备份数据压缩与解压能力
// 声明压缩与解压函数；调用方负责提供算法、原始长度以及错误输出。
// 根据指定算法处理字节数据，并在解压时检查格式、数据长度和错误
#pragma once
#include "core/archive/archive.h"
#include <vector>
namespace dbackup::compression {
// 按指定算法压缩输入；无效参数或编码失败时返回错误
bool encode(Compression, const std::vector<uint8_t>&, std::vector<uint8_t>&, std::string&);
// 解压到预期长度；长度超限、数据截断或格式错误时拒绝输入
bool decode(Compression, const std::vector<uint8_t>&, size_t, std::vector<uint8_t>&, std::string&);
} // namespace dbackup::compression

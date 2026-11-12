// 实现备份外层的分块封装与读取
// 声明分块封装的流式读写接口；调用方提供文件描述符、算法和可选密码。
// 对数据块执行可选压缩和认证加密，并校验数据完整性、顺序及结束记录
#pragma once

#include "core/archive/archive.h"
#include <string>

namespace dbackup::archive_blocks {

// 将打包数据分块封装，可按选项压缩和加密；描述符由调用方管理并负责暂存及原子提交
// 两个描述符开始时均须位于各自数据流的开头
bool encode(int packedFd, int archiveFd, const PackOptions& options,
            const OperationContext& context, std::string& error);
// 解码并验证分块封装，按需要解密和解压，返回原始打包格式
bool decode(int archiveFd, int packedFd, const std::string& password, PackFormat& format,
            const OperationContext& context, std::string& error);

} // namespace dbackup::archive_blocks

// 读写 POSIX tar 与 pax 格式
// 提供 tar 数据和文件描述符的编码、解码入口，供备份管线和增量版本复用。
// 处理条目头、扩展属性、时间和内容偏移，并拒绝格式错误或不安全的路径
#pragma once
#include "core/archive/archive_internal.h"
namespace dbackup::archive_tar {
// 将备份条目编码成可与常见 tar 工具互通的 POSIX pax 字节流
bool encode(const std::vector<archive_format::Entry>&, std::vector<uint8_t>&, std::string&);
// 仅编码一个条目的 pax 和 ustar 头；调用方另行写入文件内容及 512 字节对齐填充
bool encodeEntryHeaders(const archive_format::Entry&, uint64_t dataSize, size_t index,
                        std::vector<uint8_t>&, std::string&);
// 解析 POSIX tar/pax 数据，拒绝损坏记录及不安全路径
bool decode(const std::vector<uint8_t>&, std::vector<archive_format::Entry>&, std::string&);
// 从可定位的 tar 文件读取元数据而不加载内容，并返回各条目内容的偏移量
bool decodeMetadata(int fd, std::vector<archive_format::Entry>&, std::vector<uint64_t>& offsets,
                    std::string&);
} // namespace dbackup::archive_tar

// 处理自定义备份的流式条目格式
// 定义自定义流中条目元数据的编解码接口，避免检查大型文件时将内容全部载入内存。
// 编写条目元数据头，并在不加载完整文件内容时解析条目及其数据偏移
#pragma once
#include "core/archive/archive_internal.h"

namespace dbackup::archive_custom_stream {
// 为一个条目生成记录头，记录受限大小的元数据和后续文件内容长度
bool entryHeader(const archive_format::Entry&, uint64_t, std::vector<uint8_t>&, std::string&);
// 读取自定义备份的条目元数据及内容偏移，不加载文件内容
bool decodeMetadata(int fd, std::vector<archive_format::Entry>&, std::vector<uint64_t>&,
                    std::string&);
} // namespace dbackup::archive_custom_stream

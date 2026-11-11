#pragma once
#include "core/archive_internal.h"
namespace dbackup::archive_tar {
// 将归档条目编码成可与常见 tar 工具互通的 POSIX pax 字节流。
bool encode(const std::vector<archive_format::Entry>&, std::vector<uint8_t>&, std::string&);
// Encode only the pax and ustar headers for one entry. The caller writes its
// regular-file bytes and 512-byte padding separately.
bool encodeEntryHeaders(const archive_format::Entry&, uint64_t dataSize, size_t index,
                        std::vector<uint8_t>&, std::string&);
// 解析 POSIX tar/pax 数据，拒绝损坏记录及不安全路径。
bool decode(const std::vector<uint8_t>&, std::vector<archive_format::Entry>&, std::string&);
// Parse a seekable tar file without loading file contents; offsets correspond to entries.
bool decodeMetadata(int fd, std::vector<archive_format::Entry>&, std::vector<uint64_t>& offsets,
                    std::string&);
} // namespace dbackup::archive_tar

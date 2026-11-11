#pragma once
#include "core/archive_internal.h"

namespace dbackup::archive_custom_stream {
// Record header contains a bounded metadata object and the subsequent file size.
bool entryHeader(const archive_format::Entry&, uint64_t, std::vector<uint8_t>&, std::string&);
bool decodeMetadata(int fd, std::vector<archive_format::Entry>&, std::vector<uint64_t>&,
                    std::string&);
} // namespace dbackup::archive_custom_stream

#pragma once
#include "core/archive_internal.h"
namespace dbackup::archive_tar {
bool encode(const std::vector<archive_format::Entry>&, std::vector<uint8_t>&, std::string&);
bool decode(const std::vector<uint8_t>&, std::vector<archive_format::Entry>&, std::string&);
}

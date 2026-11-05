#pragma once
#include "core/archive.h"
#include <vector>
namespace dbackup::compression {
bool encode(Compression, const std::vector<uint8_t>&, std::vector<uint8_t>&, std::string&);
bool decode(Compression, const std::vector<uint8_t>&, size_t, std::vector<uint8_t>&, std::string&);
}

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace dbackup::crypto {
std::string md5(const uint8_t* data, size_t size);
bool md5File(const std::string& path, std::string& digest, std::string& error);
}

#pragma once

#include "core/archive.h"
#include <string>

namespace dbackup::archive_blocks {

// File descriptors must be positioned at the beginning of their respective streams.
// The caller owns both descriptors and is responsible for staging and atomic commit.
bool encode(int packedFd, int archiveFd, const PackOptions& options,
            const OperationContext& context, std::string& error);
bool decode(int archiveFd, int packedFd, const std::string& password, PackFormat& format,
            const OperationContext& context, std::string& error);

} // namespace dbackup::archive_blocks

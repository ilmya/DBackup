#pragma once

#include "core/archive.h"
#include "core/archive_internal.h"
#include "core/incremental_diff.h"

namespace dbackup::incremental {

// A partial scan is an error: its missing paths cannot safely become deletions.
// Regular-file contents are hashed with SHA-256 to detect same-metadata changes.
bool scan(const std::vector<std::string>& sources, const FilterOptions& filter,
          const OperationContext& context, std::vector<FileState>& states, std::string& error);

// Return source entries and their hashed states from the same scan. Neither
// output is modified if scanning or hashing fails.
bool scanForArchive(const std::vector<std::string>& sources, const FilterOptions& filter,
                    const OperationContext& context, std::vector<archive_scan::Entry>& entries,
                    std::vector<FileState>& states, std::string& error);

} // namespace dbackup::incremental

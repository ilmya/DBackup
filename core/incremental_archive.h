#pragma once

#include "core/archive_internal.h"
#include "core/incremental_manifest.h"

namespace dbackup::incremental {

// Write selected scanned paths as a custom or tar archive. The caller must provide
// a complete, validated scan and arrange to commit its manifest separately.
// Partial reads are never published as a successful archive.
bool writeSelectedArchive(const std::vector<archive_scan::Entry>& scanned,
                          const std::vector<std::string>& selected,
                          const std::string& destination, const PackOptions& options,
                          const OperationContext& context, std::string& error);

// Verify archive metadata and streamed regular-file SHA-256 digests against the
// manifest. Callers must prevent archive replacement before any later restore.
// The manifest itself must already be authenticated when encryption is used.
bool verifyArchive(const std::string& path, const std::string& password,
                   const Manifest& current, const Manifest* previous,
                   const OperationContext& context, std::string& error);

// Extract a previously verified archive relative to an open directory.
bool extractArchiveToFd(const std::string& path, int destinationFd, const std::string& password,
                        const OperationContext& context, std::string& error);

} // namespace dbackup::incremental

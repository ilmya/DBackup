#pragma once

#include "core/incremental_diff.h"
#include "core/archive.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::incremental {

// Each manifest holds a complete post-backup state and explicit deletion records.
struct Manifest {
    // Filesystem name is populated when reading a chain and is not serialized.
    std::string directoryName;
    uint64_t sequence = 0;
    uint64_t previous = 0;
    int64_t createdAtMs = 0;
    bool full = false;
    std::vector<FileState> files;
    std::vector<std::string> deleted;
};

// The checksum detects accidental corruption; callers must authenticate/encrypt
// the encoded bytes when using an encrypted backup.
bool encodeManifest(const Manifest& manifest, std::vector<uint8_t>& bytes, std::string& error);
bool decodeManifest(const std::vector<uint8_t>& bytes, Manifest& manifest, std::string& error);
// Password-protected form uses AES-256-GCM; no implicit plaintext fallback.
bool encodeProtectedManifest(const Manifest& manifest, const std::string& password,
                             std::vector<uint8_t>& bytes, std::string& error);
bool decodeProtectedManifest(const std::vector<uint8_t>& bytes, const std::string& password,
                             Manifest& manifest, std::string& error);

// Validate ordered versions, dependencies, and deletion records across a chain.
bool validateChain(const std::vector<Manifest>& chain, std::string& error);

// Return the number of oldest versions that can be removed while retaining at
// least keepLatest versions and the full backup they depend on.
bool removablePrefix(const std::vector<Manifest>& chain, size_t keepLatest, size_t& count,
                     std::string& error);

// Compare archive metadata against the files expected for this version.
// This does not authenticate file contents or the manifest itself.
bool validateArchiveEntries(const Manifest& current, const Manifest* previous,
                            const std::vector<ArchiveEntry>& entries, std::string& error);

} // namespace dbackup::incremental

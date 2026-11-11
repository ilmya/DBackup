#include "core/incremental_archive.h"
#include "core/archive_blocks.h"
#include "core/archive_tar.h"
#include "core/archive_custom_stream.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dbackup::incremental {
namespace {
bool digestRange(int fd, uint64_t offset, uint64_t length, const OperationContext& context,
                 std::string& digest, std::string& error) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md || EVP_DigestInit_ex(md.get(), EVP_sha256(), nullptr) != 1) {
        error = "无法初始化归档摘要";
        return false;
    }
    std::array<unsigned char, 128 * 1024> buffer{};
    while (length) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const auto size = static_cast<size_t>(std::min<uint64_t>(length, buffer.size()));
        ssize_t readBytes;
        do {
            readBytes = pread(fd, buffer.data(), size, static_cast<off_t>(offset));
        } while (readBytes < 0 && errno == EINTR);
        if (readBytes <= 0 || EVP_DigestUpdate(md.get(), buffer.data(), readBytes) != 1) {
            error = "读取或计算归档数据摘要失败";
            return false;
        }
        offset += static_cast<uint64_t>(readBytes);
        length -= static_cast<uint64_t>(readBytes);
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(md.get(), bytes.data(), &size) != 1 || size != 32) {
        error = "计算归档摘要失败";
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    digest.clear();
    for (unsigned int i = 0; i < size; ++i) {
        digest.push_back(hex[bytes[i] >> 4]);
        digest.push_back(hex[bytes[i] & 15]);
    }
    return true;
}
} // namespace

bool verifyArchive(const std::string& path, const std::string& password,
                   const Manifest& current, const Manifest* previous,
                   const OperationContext& context, std::string& error) {
    error.clear();
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    struct File {
        int fd;
        ~File() { if (fd >= 0) close(fd); }
    } source{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info {};
    if (source.fd < 0 || fstat(source.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "无法打开增量归档或归档不是普通文件";
        return false;
    }
    std::array<unsigned char, 8> magic{};
    ssize_t n;
    do { n = pread(source.fd, magic.data(), magic.size(), 0); } while (n < 0 && errno == EINTR);
    const unsigned char wrapped[] = {'D', 'B', 'P', 'K', 2, 0, 0, 0};
    const bool isWrapped = n == 8 && (!std::memcmp(magic.data(), wrapped, 8) ||
                                      !std::memcmp(magic.data(), "DBPK0002", 8));
    struct Staging {
        FILE* file = nullptr;
        ~Staging() { if (file) std::fclose(file); }
    } staging;
    int packed = source.fd;
    PackFormat format = n == 8 && !std::memcmp(magic.data(), "DBCS0001", 8)
                            ? PackFormat::Custom : PackFormat::Tar;
    if (isWrapped) {
        staging.file = std::tmpfile();
        if (!staging.file) {
            error = "无法创建归档校验暂存文件";
            return false;
        }
        packed = fileno(staging.file);
        if (!archive_blocks::decode(source.fd, packed, password, format, context, error))
            return false;
    }
    std::vector<archive_format::Entry> decoded;
    std::vector<uint64_t> offsets;
    if (!(format == PackFormat::Tar ? archive_tar::decodeMetadata(packed, decoded, offsets, error)
                                    : archive_custom_stream::decodeMetadata(packed, decoded, offsets, error)))
        return false;
    std::vector<ArchiveEntry> entries;
    entries.reserve(decoded.size());
    for (const auto& item : decoded) {
        ArchiveEntry e{};
        e.type = item.file.type;
        e.relativePath = item.file.relativePath;
        e.originalSize = e.type == 0 ? item.file.metadata.size : 0;
        e.linkTarget = item.file.linkTarget;
        entries.push_back(std::move(e));
    }
    if (!validateArchiveEntries(current, previous, entries, error))
        return false;
    std::map<std::string, const FileState*> states;
    for (const auto& f : current.files)
        states.emplace(f.path, &f);
    std::map<std::string, std::string> verifiedFiles;
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const auto& f = decoded[i].file;
        const FileState& expected = *states.at(f.relativePath);
        if (f.metadata.mode != expected.mode || f.metadata.uid != expected.uid ||
            f.metadata.gid != expected.gid || f.metadata.mtimeNs != expected.mtimeNs ||
            ((f.type == 5 || f.type == 6) &&
             (f.metadata.deviceMajor != expected.deviceMajor ||
              f.metadata.deviceMinor != expected.deviceMinor))) {
            error = "归档元数据与清单不一致: " + f.relativePath;
            return false;
        }
        if (f.type == 0) {
            std::string digest;
            if (!digestRange(packed, offsets[i], f.metadata.size, context, digest, error))
                return false;
            if (digest != expected.contentDigest) {
                error = "归档文件摘要与清单不一致: " + f.relativePath;
                return false;
            }
            verifiedFiles.emplace(f.relativePath, std::move(digest));
        } else if (f.type == 3) {
            auto target = verifiedFiles.find(f.linkTarget);
            if (target == verifiedFiles.end() || target->second != expected.contentDigest) {
                error = "归档硬链接目标或摘要无效: " + f.relativePath;
                return false;
            }
        }
    }
    struct stat after {};
    if (fstat(source.fd, &after) != 0 || info.st_size != after.st_size ||
        info.st_mtim.tv_sec != after.st_mtim.tv_sec || info.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        info.st_ctim.tv_sec != after.st_ctim.tv_sec || info.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        error = "校验期间归档已改变";
        return false;
    }
    return true;
}
} // namespace dbackup::incremental

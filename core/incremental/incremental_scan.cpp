// 为增量备份扫描来源并计算文件摘要
// 在同一轮来源扫描中收集条目和内容摘要，读取失败时返回错误而不生成不完整状态。
// 在完整扫描中生成备份条目与文件状态，避免将读取失败误判为删除
#include "core/incremental/incremental_scan.h"
#include "core/archive/archive_internal.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace dbackup::incremental {
namespace {
// 读取普通文件并计算 SHA-256，同时检查取消以及扫描期间文件变化
bool digestFile(const archive_scan::Entry& entry, const OperationContext& context,
                std::string& digest, std::string& error) {
    struct File {
        int fd;
        ~File() { if (fd >= 0) close(fd); }
    } file{open(entry.source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    if (file.fd < 0) {
        error = "无法读取增量备份来源: " + entry.source.string() + ": " + std::strerror(errno);
        return false;
    }
    struct stat before {};
    const auto& m = entry.metadata;
    if (fstat(file.fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        uint64_t(before.st_dev) != m.device || uint64_t(before.st_ino) != m.inode ||
        uint64_t(before.st_size) != m.size || uint32_t(before.st_mode) != m.mode ||
        uint32_t(before.st_uid) != m.uid || uint32_t(before.st_gid) != m.gid ||
        int64_t(before.st_mtim.tv_sec) * 1000000000LL + before.st_mtim.tv_nsec != m.mtimeNs) {
        error = "增量扫描期间文件已改变: " + entry.source.string();
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) {
        error = "无法初始化文件摘要";
        return false;
    }
    std::array<unsigned char, 128 * 1024> buffer{};
    for (;;) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const ssize_t n = read(file.fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            error = "读取增量备份来源失败: " + entry.source.string() + ": " + std::strerror(errno);
            return false;
        }
        if (n == 0)
            break;
        if (EVP_DigestUpdate(hash.get(), buffer.data(), static_cast<size_t>(n)) != 1) {
            error = "计算文件摘要失败";
            return false;
        }
    }
    struct stat after {};
    if (fstat(file.fd, &after) != 0 || before.st_size != after.st_size ||
        before.st_mode != after.st_mode || before.st_uid != after.st_uid ||
        before.st_gid != after.st_gid || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        error = "计算摘要期间文件已改变: " + entry.source.string();
        return false;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(hash.get(), bytes.data(), &length) != 1 || length != 32) {
        error = "计算文件摘要失败";
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        digest.push_back(hex[bytes[i] >> 4]);
        digest.push_back(hex[bytes[i] & 15]);
    }
    return true;
}
} // namespace

// 扫描来源并计算摘要，同时返回备份条目和文件状态
bool scanForArchive(const std::vector<std::string>& sources, const FilterOptions& filter,
                    const OperationContext& context, std::vector<archive_scan::Entry>& entries,
                    std::vector<FileState>& states, std::string& error) {
    std::vector<archive_scan::Entry> scanned;
    std::vector<std::string> warnings;
    OperationContext scanContext = context;
    scanContext.warnings = &warnings;
    const bool ok = archive_scan::collect(sources, {}, filter, scanned, nullptr, scanContext,
                                          error, true);
    if (context.warnings)
        context.warnings->insert(context.warnings->end(), warnings.begin(), warnings.end());
    if (!ok)
        return false;
    if (!warnings.empty()) {
        error = "增量扫描未完成，不能判断删除项: " + warnings.front();
        return false;
    }
    std::vector<FileState> result;
    result.reserve(scanned.size());
    for (const auto& entry : scanned) {
        const auto& meta = entry.metadata;
        FileState state;
        state.path = entry.relativePath;
        state.type = entry.type;
        state.mode = meta.mode;
        state.uid = meta.uid;
        state.gid = meta.gid;
        state.mtimeNs = meta.mtimeNs;
        state.size = meta.size;
        state.deviceMajor = meta.deviceMajor;
        state.deviceMinor = meta.deviceMinor;
        state.linkTarget = entry.linkTarget;
        if ((entry.type == 0 || entry.type == 3) &&
            !digestFile(entry, context, state.contentDigest, error))
            return false;
        result.push_back(std::move(state));
    }
    entries = std::move(scanned);
    states = std::move(result);
    error.clear();
    return true;
}

// 扫描来源并生成文件状态，供增量版本比较使用
bool scan(const std::vector<std::string>& sources, const FilterOptions& filter,
          const OperationContext& context, std::vector<FileState>& states, std::string& error) {
    std::vector<archive_scan::Entry> entries;
    return scanForArchive(sources, filter, context, entries, states, error);
}
} // namespace dbackup::incremental

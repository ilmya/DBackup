#include "core/archive.h"
#include "core/archive_internal.h"
#include "core/archive_blocks.h"
#include "core/archive_custom_stream.h"
#include "core/archive_tar.h"
#include "core/backup_policy.h"
#include "core/crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace {
using dbackup::archive_format::Entry;
namespace fs = std::filesystem;

// 仅打开扫描阶段确认过的普通文件，并校验读取前后元数据未变化。
bool readSource(const dbackup::archive_scan::Entry& entry, std::vector<uint8_t>& data,
                const OperationContext& context, std::string& error, int output = -1) {
    std::vector<char> buffer(4u * 1024u * 1024u);
    int fd = open(entry.source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        error = "无法读取文件: " + entry.source.string() + ": " + std::strerror(errno);
        return false;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        uint64_t(info.st_dev) != entry.metadata.device ||
        uint64_t(info.st_ino) != entry.metadata.inode ||
        uint64_t(info.st_size) != entry.metadata.size ||
        uint32_t(info.st_mode) != entry.metadata.mode ||
        uint32_t(info.st_uid) != entry.metadata.uid ||
        uint32_t(info.st_gid) != entry.metadata.gid ||
        int64_t(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec !=
            entry.metadata.mtimeNs) {
        close(fd);
        error = "扫描后文件已改变: " + entry.source.string();
        return false;
    }
    for (;;) {
        if (context.isCancelled()) {
            close(fd);
            error = "操作已取消";
            return false;
        }
        ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            error = "读取文件失败: " + entry.source.string() + ": " + std::strerror(errno);
            close(fd);
            return false;
        }
        if (count == 0)
            break;
        if (output < 0) {
            data.insert(data.end(), buffer.data(), buffer.data() + count);
        } else {
            size_t written = 0;
            while (written < static_cast<size_t>(count)) {
                ssize_t n = write(output, buffer.data() + written, size_t(count) - written);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    error = "写入 tar 文件失败: " + std::string(std::strerror(errno));
                    close(fd);
                    return false;
                }
                written += static_cast<size_t>(n);
            }
        }
    }
    struct stat end {};
    const bool unchanged =
        fstat(fd, &end) == 0 && end.st_size == info.st_size && end.st_mode == info.st_mode &&
        end.st_uid == info.st_uid && end.st_gid == info.st_gid &&
        end.st_mtim.tv_sec == info.st_mtim.tv_sec && end.st_mtim.tv_nsec == info.st_mtim.tv_nsec &&
        end.st_ctim.tv_sec == info.st_ctim.tv_sec && end.st_ctim.tv_nsec == info.st_ctim.tv_nsec;
    close(fd);
    if (!unchanged) {
        error = "备份期间文件已改变: " + entry.source.string();
        return false;
    }
    return true;
}

// 以二进制方式读取归档文件，失败时保留明确的文件路径错误。
bool readArchive(const std::string& path, std::vector<uint8_t>& data, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "无法打开归档: " + path;
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        error = "读取归档失败: " + path;
        return false;
    }
    return true;
}

// 在目标目录写入临时归档并同步磁盘，最后原子提交或拒绝覆盖。
bool writeArchive(const std::string& path, const std::vector<uint8_t>& data,
                  const OperationContext& context, std::string& error) {
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) {
        error = "无法创建临时归档: " + std::string(std::strerror(errno));
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < data.size()) {
        if (context.isCancelled()) {
            error = "操作已取消";
            ok = false;
            break;
        }
        ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = "写入归档失败: " + std::string(std::strerror(errno));
            ok = false;
            break;
        }
        written += static_cast<size_t>(count);
    }
    if (ok && fsync(fd) != 0) {
        error = "刷新归档失败: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (close(fd) != 0 && ok) {
        error = "关闭归档失败: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (ok && context.isCancelled()) {
        error = "操作已取消";
        ok = false;
    }
    if (ok) {
        const int result = context.noReplace
                               ? static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, name.data(),
                                                          AT_FDCWD, path.c_str(), 1u))
                               : rename(name.data(), path.c_str());
        if (result != 0) {
            error = "提交归档失败: " + std::string(std::strerror(errno));
            ok = false;
        }
    }
    if (!ok)
        unlink(name.data());
    return ok;
}

bool createPlainTar(const std::vector<dbackup::archive_scan::Entry>& scanned,
                    const std::string& destination, const OperationContext& context,
                    OperationContext& scanContext, size_t warningStart, std::string& error,
                    PackFormat format = PackFormat::Tar) {
    std::string pattern = destination + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    struct Temporary {
        int fd;
        const char* name;
        bool committed = false;
        bool created = false;
        ~Temporary() {
            if (fd >= 0) close(fd);
            if (created && !committed) unlink(name);
        }
    } temporary{mkstemp(name.data()), name.data()};
    if (temporary.fd < 0) {
        error = "无法创建临时归档: " + std::string(std::strerror(errno));
        return false;
    }
    temporary.created = true;
    auto writeBytes = [&](const uint8_t* bytes, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            if (context.isCancelled()) {
                error = "操作已取消";
                return false;
            }
            ssize_t count = write(temporary.fd, bytes + offset, size - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                error = "写入 tar 归档失败: " + std::string(std::strerror(errno));
                return false;
            }
            offset += static_cast<size_t>(count);
        }
        return true;
    };
    if (format == PackFormat::Custom) {
        const uint8_t magic[] = {'D','B','C','S','0','0','0','1'};
        if (!writeBytes(magic, sizeof(magic))) return false;
    }
    std::set<std::string> availableFiles;
    size_t count = 0;
    OperationProgress progress;
    progress.stage = OperationStage::Reading;
    progress.totalFiles = scanned.size();
    for (const auto& item : scanned) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        if (item.type == 3 && !availableFiles.count(item.linkTarget)) {
            scanContext.warn(item.relativePath + ": 硬链接目标未能备份");
            continue;
        }
        off_t start = lseek(temporary.fd, 0, SEEK_CUR);
        if (start < 0) {
            error = "无法定位临时归档";
            return false;
        }
        Entry entry;
        entry.file = item;
        const uint64_t size = item.type == 0 ? item.metadata.size : 0;
        std::vector<uint8_t> headers;
        const bool headersOk = format == PackFormat::Tar
            ? dbackup::archive_tar::encodeEntryHeaders(entry, size, count, headers, error)
            : dbackup::archive_custom_stream::entryHeader(entry, size, headers, error);
        if (!headersOk ||
            !writeBytes(headers.data(), headers.size()))
            return false;
        std::vector<uint8_t> unused;
        errno = 0;
        if (item.type == 0 && !readSource(item, unused, context, error, temporary.fd)) {
            if (errno == EACCES || errno == EPERM) {
                if (ftruncate(temporary.fd, start) != 0 || lseek(temporary.fd, start, SEEK_SET) < 0) {
                    error = "无法回退未读取的 tar 条目";
                    return false;
                }
                scanContext.warn(error);
                error.clear();
                continue;
            }
            return false;
        }
        static const uint8_t zeros[1024]{};
        const size_t padding = format == PackFormat::Tar
            ? static_cast<size_t>((512 - size % 512) % 512) : 0;
        if (!writeBytes(zeros, padding)) return false;
        if (item.type == 0) availableFiles.insert(item.relativePath);
        ++count;
        progress.currentPath = item.relativePath;
        progress.completedFiles = count;
        context.report(progress);
    }
    if (!count && scanContext.warnings->size() > warningStart) {
        error = "没有可备份的内容";
        for (size_t i = warningStart; i < scanContext.warnings->size(); ++i)
            error += "\n" + (*scanContext.warnings)[i];
        return false;
    }
    static const uint8_t end[1024]{};
    if (!writeBytes(end, format == PackFormat::Tar ? sizeof(end) : 8) ||
        fsync(temporary.fd) != 0) {
        if (error.empty()) error = "刷新归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    int fd = temporary.fd;
    temporary.fd = -1;
    if (close(fd) != 0) {
        error = "关闭归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    int result = context.noReplace
        ? static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, name.data(), AT_FDCWD,
                                   destination.c_str(), 1u))
        : rename(name.data(), destination.c_str());
    if (result != 0) {
        error = "提交归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    temporary.committed = true;
    for (size_t i = warningStart; i < scanContext.warnings->size(); ++i) {
        if (!error.empty()) error += "\n";
        error += (*scanContext.warnings)[i];
    }
    progress.stage = OperationStage::Completed;
    context.report(progress);
    return true;
}

bool createWrapped(const std::vector<dbackup::archive_scan::Entry>& scanned,
                   const std::string& destination, const PackOptions& options,
                   const OperationContext& context, OperationContext& scanContext,
                   size_t warningStart, std::string& error) {
    struct TemporaryPath {
        std::vector<char> name;
        int fd = -1;
        bool created = false;
        explicit TemporaryPath(const std::string& pattern) : name(pattern.begin(), pattern.end()) {
            name.push_back('\0');
            fd = mkstemp(name.data());
            created = fd >= 0;
        }
        ~TemporaryPath() {
            if (fd >= 0) close(fd);
            if (created) unlink(name.data());
        }
    } packed(destination + ".packed.XXXXXX"), output(destination + ".tmp.XXXXXX");
    if (packed.fd < 0 || output.fd < 0) {
        error = "无法创建归档暂存文件: " + std::string(std::strerror(errno));
        return false;
    }
    close(packed.fd);
    packed.fd = -1;
    OperationContext packing = context;
    packing.onProgress = [&context](const OperationProgress& progress) {
        if (progress.stage != OperationStage::Completed)
            context.report(progress);
    };
    // The reserved staging path already exists; noReplace applies only to the
    // caller's final archive, not to this private temporary file.
    packing.noReplace = false;
    if (!createPlainTar(scanned, packed.name.data(), packing, scanContext, warningStart, error,
                        options.format))
        return false;
    packed.fd = open(packed.name.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (packed.fd < 0) {
        error = "无法读取归档暂存文件: " + std::string(std::strerror(errno));
        return false;
    }
    OperationProgress progress;
    progress.stage = OperationStage::Compressing;
    context.report(progress);
    std::string warnings = error;
    error.clear();
    PackOptions effectiveOptions = options;
    if (effectiveOptions.compression == Compression::Deflate && effectiveOptions.compressionLevel == 0)
        effectiveOptions.compression = Compression::None;
    if (!dbackup::archive_blocks::encode(packed.fd, output.fd, effectiveOptions, context, error))
        return false;
    if (fsync(output.fd) != 0) {
        error = "刷新归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    int fd = output.fd;
    output.fd = -1;
    if (close(fd) != 0) {
        error = "关闭归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    const int result = context.noReplace
        ? static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, output.name.data(), AT_FDCWD,
                                   destination.c_str(), 1u))
        : rename(output.name.data(), destination.c_str());
    if (result != 0) {
        error = "提交归档失败: " + std::string(std::strerror(errno));
        return false;
    }
    output.created = false;
    error = std::move(warnings);
    progress.stage = OperationStage::Completed;
    context.report(progress);
    return true;
}

// 读取当前归档文件并执行格式识别、解密和校验。
bool decodeFile(const std::string& path, const std::string& password, std::vector<Entry>& entries,
                std::string& error) {
    std::vector<uint8_t> bytes;
    return readArchive(path, bytes, error) &&
           dbackup::archive_format::decodeCurrent(bytes, password, entries, error);
}

// Restore a seekable tar by referring to its file ranges instead of retaining
// their contents in the decoded metadata list.
bool restoreTar(int fd, const std::string& destination, const OperationContext& context,
                std::string& error, PackFormat format = PackFormat::Tar) {
    std::vector<Entry> entries;
    std::vector<uint64_t> offsets;
    const bool decoded = format == PackFormat::Tar
        ? dbackup::archive_tar::decodeMetadata(fd, entries, offsets, error)
        : dbackup::archive_custom_stream::decodeMetadata(fd, entries, offsets, error);
    if (!decoded)
        return false;
    std::map<std::string, size_t> indices;
    for (size_t i = 0; i < entries.size(); ++i)
        indices.emplace(entries[i].file.relativePath, i);
    const auto writer = [&](const Entry& entry, int output, const OperationContext& operation,
                            std::string& detail) {
        auto found = indices.find(entry.file.relativePath);
        if (found == indices.end()) {
            detail = "归档条目缺少数据位置";
            errno = EIO;
            return false;
        }
        uint64_t position = offsets[found->second], left = entry.file.metadata.size;
        std::vector<uint8_t> buffer(4u * 1024u * 1024u);
        while (left) {
            if (operation.isCancelled()) {
                detail = "操作已取消";
                return false;
            }
            size_t requested = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
            ssize_t count;
            do { count = pread(fd, buffer.data(), requested, static_cast<off_t>(position)); }
            while (count < 0 && errno == EINTR);
            if (count <= 0) {
                detail = "读取 tar 文件数据失败: " + entry.file.relativePath;
                errno = EIO;
                return false;
            }
            size_t done = 0;
            while (done < static_cast<size_t>(count)) {
                ssize_t written = write(output, buffer.data() + done, size_t(count) - done);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0) {
                    detail = "写入恢复文件失败: " + entry.file.relativePath + ": " +
                             std::strerror(errno);
                    return false;
                }
                done += static_cast<size_t>(written);
            }
            position += static_cast<size_t>(count);
            left -= static_cast<size_t>(count);
        }
        return true;
    };
    return dbackup::archive_restore::restore(entries, destination, context, error, writer);
}

bool inspectPacked(const std::string& file, std::vector<ArchiveEntry>& output, std::string& error,
                   const std::string& password, bool includeData) {
    output.clear();
    error.clear();
    struct CloseFd {
        int fd;
        ~CloseFd() { if (fd >= 0) close(fd); }
    } source{open(file.c_str(), O_RDONLY | O_CLOEXEC)};
    if (source.fd < 0) {
        error = "无法打开归档: " + file;
        return false;
    }
    uint8_t magic[10]{};
    ssize_t n;
    do { n = pread(source.fd, magic, sizeof(magic), 0); } while (n < 0 && errno == EINTR);
    if (n >= 8 && std::memcmp(magic, "DBPK0001", 8) == 0) {
        error = "不支持旧版 DBackup 封装格式";
        return false;
    }
    struct CloseStream {
        FILE* stream = nullptr;
        ~CloseStream() { if (stream) std::fclose(stream); }
    } staging;
    int packedFd = source.fd;
    PackFormat format = PackFormat::Tar;
    bool compressed = false;
    const uint8_t currentMagic[] = {'D', 'B', 'P', 'K', 2, 0, 0, 0};
    if (n >= 8 && (!std::memcmp(magic, currentMagic, 8) ||
                   !std::memcmp(magic, "DBPK0002", 8))) {
        staging.stream = std::tmpfile();
        if (!staging.stream) {
            error = "无法创建归档暂存文件: " + std::string(std::strerror(errno));
            return false;
        }
        packedFd = fileno(staging.stream);
        if (!dbackup::archive_blocks::decode(source.fd, packedFd, password, format, {}, error))
            return false;
        compressed = n >= 10 && magic[9] != uint8_t(Compression::None);
    }
    std::vector<Entry> decoded;
    std::vector<uint64_t> offsets;
    bool ok = format == PackFormat::Tar
        ? dbackup::archive_tar::decodeMetadata(packedFd, decoded, offsets, error)
        : dbackup::archive_custom_stream::decodeMetadata(packedFd, decoded, offsets, error);
    if (!ok) return false;
    constexpr uint64_t maxContent = 64u * 1024u * 1024u;
    uint64_t total = 0;
    for (const auto& item : decoded) {
        if (!includeData || item.file.type != 0) continue;
        if (item.file.metadata.size > maxContent - total) {
            error = "归档文件内容超过 64 MiB 检查限制，请使用元数据检查";
            return false;
        }
        total += item.file.metadata.size;
    }
    std::vector<ArchiveEntry> result;
    for (size_t i = 0; i < decoded.size(); ++i) {
        const auto& item = decoded[i];
        ArchiveEntry entry{};
        entry.type = item.file.type;
        entry.flags = compressed && item.file.type == 0 ? 1 : 0;
        entry.relativePath = item.file.relativePath;
        entry.mode = item.file.metadata.mode;
        entry.mtimeMs = item.file.metadata.mtimeNs / 1000000;
        entry.uid = item.file.metadata.uid;
        entry.gid = item.file.metadata.gid;
        entry.deviceMajor = item.file.metadata.deviceMajor;
        entry.deviceMinor = item.file.metadata.deviceMinor;
        entry.owner = item.file.owner;
        entry.linkTarget = item.file.linkTarget;
        entry.originalSize = item.file.type == 0 ? item.file.metadata.size : 0;
        if (includeData && entry.originalSize) {
            entry.data.resize(static_cast<size_t>(entry.originalSize));
            size_t done = 0;
            while (done < entry.data.size()) {
                ssize_t count = pread(packedFd, &entry.data[done], entry.data.size() - done,
                                      static_cast<off_t>(offsets[i] + done));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    error = "读取归档内容失败: " + entry.relativePath;
                    return false;
                }
                done += static_cast<size_t>(count);
            }
        }
        result.push_back(std::move(entry));
    }
    output = std::move(result);
    return true;
}
} // namespace

bool ArchiveService::create(const std::string& source, const std::string& destination,
                            std::string& error) {
    return create(source, destination, PackOptions{}, error);
}
bool ArchiveService::create(const std::string& source, const std::string& destination,
                            const PackOptions& options, std::string& error) {
    return create(std::vector<std::string>{source}, destination, options, error, {});
}
bool ArchiveService::create(const std::vector<std::string>& sources, const std::string& destination,
                            const PackOptions& options, std::string& error) {
    return create(sources, destination, options, error, {});
}
bool ArchiveService::create(const std::vector<std::string>& sources, const std::string& destination,
                            const PackOptions& options, std::string& error,
                            const OperationContext& context) try {
    error.clear();
    std::vector<std::string> localWarnings;
    OperationContext scanContext = context;
    if (!scanContext.warnings)
        scanContext.warnings = &localWarnings;
    const size_t warningStart = scanContext.warnings->size();
    std::vector<dbackup::archive_scan::Entry> scanned;
    OperationProgress progress;
    progress.stage = OperationStage::Scanning;
    context.report(progress);
    if (!dbackup::archive_scan::collect(sources, destination, options.filter, scanned, nullptr,
                                        scanContext, error, context.includeSourceDirectory))
        return false;
    const bool plainTar = options.format == PackFormat::Tar && options.password.empty() &&
                          (options.compression == Compression::None ||
                           (options.compression == Compression::Deflate && options.compressionLevel == 0));
    if (plainTar)
        return createPlainTar(scanned, destination, context, scanContext, warningStart, error);
    return createWrapped(scanned, destination, options, context, scanContext, warningStart, error);
} catch (const std::bad_alloc&) {
    error = "内存不足，无法完成备份";
    return false;
}
bool ArchiveService::preview(const std::vector<std::string>& sources, const FilterOptions& filter,
                             BackupPreview& preview, std::string& error,
                             bool includeSingleDirectoryRoot) {
    std::vector<dbackup::archive_scan::Entry> scanned;
    std::vector<std::string> warnings;
    OperationContext context;
    context.warnings = &warnings;
    if (!dbackup::archive_scan::collect(sources, {}, filter, scanned, &preview, context, error,
                                        includeSingleDirectoryRoot))
        return false;
    for (const auto& warning : warnings) {
        if (!error.empty())
            error += "\n";
        error += warning;
    }
    return true;
}
bool ArchiveService::inspect(const std::string& file, std::vector<ArchiveEntry>& entries,
                             std::string& error) {
    return inspect(file, entries, error, "");
}
bool ArchiveService::inspect(const std::string& file, std::vector<ArchiveEntry>& entries,
                             std::string& error, const std::string& password) try {
    return inspectPacked(file, entries, error, password, true);
} catch (const std::bad_alloc&) {
    entries.clear();
    error = "内存不足，无法检查归档";
    return false;
}
bool ArchiveService::inspectMetadata(const std::string& file, std::vector<ArchiveEntry>& entries,
                                     std::string& error, const std::string& password) try {
    return inspectPacked(file, entries, error, password, false);
} catch (const std::bad_alloc&) {
    entries.clear();
    error = "内存不足，无法检查归档";
    return false;
}
bool ArchiveService::extract(const std::string& file, const std::string& destination,
                             std::string& error) {
    return extract(file, destination, error, "", {});
}
bool ArchiveService::extract(const std::string& file, const std::string& destination,
                             std::string& error, const std::string& password) {
    return extract(file, destination, error, password, {});
}
bool ArchiveService::extract(const std::string& file, const std::string& destination,
                             std::string& error, const std::string& password,
                             const OperationContext& context) try {
    int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "无法打开归档: " + file;
        return false;
    }
    struct CloseFile {
        int fd;
        ~CloseFile() { if (fd >= 0) close(fd); }
    } closeFile{fd};
    uint8_t magic[8]{};
    ssize_t count;
    do { count = pread(fd, magic, sizeof(magic), 0); } while (count < 0 && errno == EINTR);
    const bool wrapped = count == 8 && std::memcmp(magic, "DBPK0001", 8) == 0;
    const uint8_t currentMagic[] = {'D', 'B', 'P', 'K', 2, 0, 0, 0};
    const bool chunked = count == 8 && (!std::memcmp(magic, currentMagic, 8) ||
                                        !std::memcmp(magic, "DBPK0002", 8));
    if (wrapped) {
        error = "不支持旧版 DBackup 封装格式";
        return false;
    }
    if (chunked) {
        struct CloseStream {
            FILE* stream;
            ~CloseStream() { if (stream) std::fclose(stream); }
        } temporary{std::tmpfile()};
        if (!temporary.stream) {
            error = "无法创建归档暂存文件: " + std::string(std::strerror(errno));
            return false;
        }
        const int packedFd = fileno(temporary.stream);
        PackFormat format;
        if (!dbackup::archive_blocks::decode(fd, packedFd, password, format, context, error))
            return false;
        return restoreTar(packedFd, destination, context, error, format);
    }
    return restoreTar(fd, destination, context, error);
} catch (const std::bad_alloc&) {
    error = "内存不足，无法恢复归档";
    return false;
}

#include "core/archive_internal.h"



#include <fnmatch.h>
#include <pwd.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <set>

namespace dbackup::archive_scan {
namespace fs = std::filesystem;
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

bool matches(const Entry &entry, const FilterOptions &filter) {
    const bool directory = entry.type == 1;
    if (filter.type == EntryTypeFilter::FilesOnly && directory) return false;
    if (filter.type == EntryTypeFilter::DirectoriesOnly && !directory) return false;
    const auto path = lower(entry.relativePath);
    const auto name = lower(fs::path(entry.relativePath).filename().string());
    bool includes = false, included = false;
    for (const auto &rule : filter.pathRules) {
        includes |= rule.include;
        if (fnmatch(lower(rule.pattern).c_str(), path.c_str(), 0) == 0) {
            if (!rule.include) return false;
            included = true;
        }
    }
    if (includes && !included) return false;
    if (!filter.pathContains.empty() && path.find(lower(filter.pathContains)) == std::string::npos) return false;
    if (!filter.nameContains.empty() && name.find(lower(filter.nameContains)) == std::string::npos) return false;
    if (!filter.ownerContains.empty() && lower(entry.owner).find(lower(filter.ownerContains)) == std::string::npos) return false;
    if (!directory) {
        std::vector<std::string> extensions = filter.extensions;
        if (extensions.empty() && !filter.extension.empty()) extensions.push_back(filter.extension);
        if (!extensions.empty()) {
            bool found = false;
            for (auto extension : extensions) {
                extension = lower(extension);
                if (!extension.empty() && extension[0] != '.') extension.insert(extension.begin(), '.');
                if (!extension.empty() && name.size() >= extension.size() &&
                    name.compare(name.size() - extension.size(), extension.size(), extension) == 0) found = true;
            }
            if (!found) return false;
        }
    }
    if (filter.minSize && entry.metadata.size < filter.minSize) return false;
    if (filter.maxSize && entry.metadata.size > filter.maxSize) return false;
    const auto mtime = entry.metadata.mtimeNs / 1000000;
    if (filter.afterMtimeMs && mtime < filter.afterMtimeMs) return false;
    if (filter.beforeMtimeMs && mtime > filter.beforeMtimeMs) return false;
    return true;
}

void addPreview(BackupPreview &result, const Entry &entry, bool included) {
    if (entry.type == 1) {
        if (included) ++result.includedDirectories; else ++result.excludedDirectories;
    } else if (included) {
        ++result.includedFiles; result.includedBytes += entry.metadata.size;
    } else {
        ++result.excludedFiles; result.excludedBytes += entry.metadata.size;
    }
}
}

bool collect(const std::vector<std::string> &sources, const fs::path &excluded,
             const FilterOptions &filter, std::vector<Entry> &entries,
             BackupPreview *preview, const OperationContext &context, std::string &error, bool includeSingleDirectoryRoot) {
    entries.clear();
    if (preview) *preview = {};
    if (sources.empty()) { error = "请至少选择一个备份来源"; return false; }
    std::error_code ec;
    const fs::path excludedAbsolute = excluded.empty() ? fs::path() : fs::absolute(excluded, ec).lexically_normal();
    if (ec) { error = ec.message(); return false; }
    std::set<fs::path> uniqueSources;
    std::set<std::string> names;
    std::map<std::pair<uint64_t, uint64_t>, std::string> hardlinks;
    auto process = [&](const fs::path &path, const fs::path &relative, bool forceDirectory) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        if (!excludedAbsolute.empty() && fs::absolute(path, ec).lexically_normal() == excludedAbsolute) return true;
        if (ec) { error = ec.message(); return false; }
        Entry entry; entry.source = path; entry.relativePath = relative.generic_string();
        if (!linux_file::readMetadata(path, entry.metadata, error)) return false;
        if (S_ISDIR(entry.metadata.mode)) entry.type = 1;
        else if (S_ISLNK(entry.metadata.mode)) entry.type = 2;
        else if (!S_ISREG(entry.metadata.mode)) { error = "不支持的特殊文件: " + path.string(); return false; }
        if (const passwd *owner = getpwuid(entry.metadata.uid)) entry.owner = owner->pw_name;
        const bool selected = forceDirectory || matches(entry, filter);
        if (preview) addPreview(*preview, entry, selected);
        if (!selected) return true;
        if (!linux_file::validRelativePath(relative) || !names.insert(entry.relativePath).second) {
            error = "备份来源包含无效或重复路径: " + entry.relativePath; return false;
        }
        if (entry.type == 2) {
            entry.linkTarget = fs::read_symlink(path, ec).generic_string();
            if (ec) { error = "无法读取符号链接: " + path.string() + ": " + ec.message(); return false; }
        } else if (entry.type == 0 && entry.metadata.links > 1) {
            const auto key = std::make_pair(entry.metadata.device, entry.metadata.inode);
            auto [it, inserted] = hardlinks.emplace(key, entry.relativePath);
            if (!inserted) { entry.type = 3; entry.linkTarget = it->second; }
        }
        entries.push_back(std::move(entry));
        return true;
    };
    for (const auto &source : sources) {
        fs::path absolute = fs::absolute(source, ec).lexically_normal();
        if (ec) { error = ec.message(); return false; }
        if (!excludedAbsolute.empty() && absolute == excludedAbsolute) {
            error = "备份源不能同时作为输出文件: " + source; return false;
        }
        if (!uniqueSources.insert(absolute).second) continue;
        linux_file::Metadata metadata;
        if (!linux_file::readMetadata(absolute, metadata, error)) return false;
        const bool directory = S_ISDIR(metadata.mode);
        const bool rootIncluded = includeSingleDirectoryRoot || sources.size() > 1 || !directory;
        fs::path rootName = absolute.filename();
        if (rootIncluded && rootName.empty()) { error = "无法使用文件系统根目录作为多个来源之一"; return false; }
        if (rootIncluded && !process(absolute, rootName, directory)) return false;
        if (!directory) continue;
        fs::recursive_directory_iterator it(absolute, fs::directory_options::none, ec), end;
        if (ec) { error = "无法扫描目录: " + absolute.string() + ": " + ec.message(); return false; }
        while (it != end) {
            fs::path relative = it->path().lexically_relative(absolute);
            if (rootIncluded) relative = rootName / relative;
            if (!process(it->path(), relative, false)) return false;
            it.increment(ec);
            if (ec) { error = "无法扫描目录: " + ec.message(); return false; }
        }
    }
    return true;
}
}  // namespace dbackup::archive_scan


#include "core/crypto.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace dbackup::archive_format {
namespace {
constexpr uint8_t magic[] = {'D','B','L','I','N','0','0','1'};
constexpr size_t digestSize = 32, saltSize = 16, nonceSize = 12, tagSize = 16;
constexpr uint64_t maxEntries = 10000000;

void put64(std::vector<uint8_t> &out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void putBytes(std::vector<uint8_t> &out, const uint8_t *data, size_t size) {
    put64(out, size);
    if (size) out.insert(out.end(), data, data + size);
}
void putString(std::vector<uint8_t> &out, const std::string &value) {
    putBytes(out, reinterpret_cast<const uint8_t *>(value.data()), value.size());
}
struct Reader {
    const std::vector<uint8_t> &bytes;
    size_t offset = 0;
    bool integer(uint64_t &value) {
        if (bytes.size() - offset < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) value |= uint64_t(bytes[offset++]) << (8 * i);
        return true;
    }
    bool data(std::vector<uint8_t> &value) {
        uint64_t size = 0;
        if (!integer(size) || size > bytes.size() - offset) return false;
        value.assign(bytes.begin() + offset, bytes.begin() + offset + static_cast<size_t>(size));
        offset += static_cast<size_t>(size);
        return true;
    }
    bool string(std::string &value) {
        std::vector<uint8_t> dataValue;
        if (!data(dataValue)) return false;
        value.assign(dataValue.begin(), dataValue.end());
        return value.find('\0') == std::string::npos;
    }
};
bool derive(const std::string &password, const std::vector<uint8_t> &salt, std::vector<uint8_t> &key) {
    key.resize(32);
    return crypto::pbkdf2Sha256(password, salt, crypto::kDefaultPbkdf2Iterations, key);
}
}

bool encode(const std::vector<Entry> &entries, int compressionLevel, const std::string &password,
            std::vector<uint8_t> &output, std::string &error) {
    if (entries.size() > maxEntries) { error = "归档条目过多"; return false; }
    std::vector<uint8_t> body;
    put64(body, entries.size());
    for (const auto &entry : entries) {
        const auto &file = entry.file;
        if (file.type > 3 || !linux_file::validRelativePath(file.relativePath)) {
            error = "归档条目路径或类型无效: " + file.relativePath; return false;
        }
        if (file.type != 0 && !entry.data.empty()) { error = "非普通文件包含数据"; return false; }
        put64(body, file.type);
        putString(body, file.relativePath);
        put64(body, file.metadata.mode);
        put64(body, file.metadata.uid);
        put64(body, file.metadata.gid);
        put64(body, static_cast<uint64_t>(file.metadata.mtimeNs));
        putString(body, file.owner);
        putString(body, file.linkTarget);
        put64(body, entry.data.size());
        std::vector<uint8_t> compressed;
        bool useCompression = false;
        if (!entry.data.empty() && entry.data.size() <= std::numeric_limits<uLong>::max()) {
            uLongf size = compressBound(static_cast<uLong>(entry.data.size()));
            compressed.resize(size);
            if (compress2(compressed.data(), &size, entry.data.data(), static_cast<uLong>(entry.data.size()),
                          std::clamp(compressionLevel, 0, 9)) != Z_OK) {
                error = "压缩文件失败: " + file.relativePath; return false;
            }
            compressed.resize(size);
            useCompression = compressed.size() < entry.data.size();
        }
        put64(body, useCompression ? 1 : 0);
        const auto &payload = useCompression ? compressed : entry.data;
        putBytes(body, payload.data(), payload.size());
    }
    std::vector<uint8_t> digest;
    if (!crypto::sha256(body.data(), body.size(), digest)) { error = "无法计算归档校验值"; return false; }
    body.insert(body.end(), digest.begin(), digest.end());
    output.assign(std::begin(magic), std::end(magic));
    output.push_back(password.empty() ? 0 : 1);
    if (!password.empty()) {
        std::vector<uint8_t> salt, nonce, tag, key, ciphertext;
        if (!crypto::randomBytes(salt, saltSize) || !crypto::randomBytes(nonce, nonceSize) ||
            !derive(password, salt, key) || !crypto::aes256Gcm(true, body, key, nonce, tag, ciphertext)) {
            error = "归档加密失败"; output.clear(); return false;
        }
        output.insert(output.end(), salt.begin(), salt.end());
        output.insert(output.end(), nonce.begin(), nonce.end());
        output.insert(output.end(), tag.begin(), tag.end());
        output.insert(output.end(), ciphertext.begin(), ciphertext.end());
    } else output.insert(output.end(), body.begin(), body.end());
    return true;
}

bool decode(const std::vector<uint8_t> &input, const std::string &password,
            std::vector<Entry> &entries, std::string &error) {
    entries.clear();
    if (input.size() < sizeof(magic) + 1 || !std::equal(std::begin(magic), std::end(magic), input.begin())) {
        error = "不是当前 Linux 版本支持的归档格式"; return false;
    }
    size_t offset = sizeof(magic);
    const uint8_t encrypted = input[offset++];
    if (encrypted > 1) { error = "归档标志无效"; return false; }
    std::vector<uint8_t> body;
    if (encrypted) {
        if (password.empty() || input.size() - offset < saltSize + nonceSize + tagSize) {
            error = "归档需要密码或数据不完整"; return false;
        }
        std::vector<uint8_t> salt(input.begin() + offset, input.begin() + offset + saltSize); offset += saltSize;
        std::vector<uint8_t> nonce(input.begin() + offset, input.begin() + offset + nonceSize); offset += nonceSize;
        std::vector<uint8_t> tag(input.begin() + offset, input.begin() + offset + tagSize); offset += tagSize;
        std::vector<uint8_t> ciphertext(input.begin() + offset, input.end()), key;
        if (!derive(password, salt, key) || !crypto::aes256Gcm(false, ciphertext, key, nonce, tag, body)) {
            error = "密码错误或归档已损坏"; return false;
        }
    } else body.assign(input.begin() + offset, input.end());
    if (body.size() < digestSize) { error = "归档数据不完整"; return false; }
    std::vector<uint8_t> actual;
    if (!crypto::sha256(body.data(), body.size() - digestSize, actual) ||
        !std::equal(actual.begin(), actual.end(), body.end() - digestSize)) {
        error = "归档校验失败"; return false;
    }
    body.resize(body.size() - digestSize);
    Reader reader{body};
    uint64_t count = 0;
    if (!reader.integer(count) || count > maxEntries) { error = "归档条目数量无效"; return false; }
    for (uint64_t i = 0; i < count; ++i) {
        Entry entry; uint64_t type, mode, uid, gid, mtime, size, compressed;
        if (!reader.integer(type) || type > 3 || !reader.string(entry.file.relativePath) ||
            !linux_file::validRelativePath(entry.file.relativePath) || !reader.integer(mode) ||
            !reader.integer(uid) || !reader.integer(gid) || !reader.integer(mtime) ||
            mode > UINT32_MAX || uid > UINT32_MAX || gid > UINT32_MAX ||
            !reader.string(entry.file.owner) || !reader.string(entry.file.linkTarget) ||
            !reader.integer(size) || !reader.integer(compressed) || compressed > 1 ||
            !reader.data(entry.data)) { error = "归档条目无效"; entries.clear(); return false; }
        entry.compressed = compressed != 0;
        entry.file.type = static_cast<uint8_t>(type);
        entry.file.metadata.mode = static_cast<uint32_t>(mode);
        entry.file.metadata.uid = static_cast<uint32_t>(uid);
        entry.file.metadata.gid = static_cast<uint32_t>(gid);
        entry.file.metadata.mtimeNs = static_cast<int64_t>(mtime);
        entry.file.metadata.size = size;
        if (type != 0 && (size || !entry.data.empty() || compressed)) {
            error = "非普通文件包含数据"; entries.clear(); return false;
        }
        if (compressed) {
            if (size > std::numeric_limits<uLongf>::max() || size > entry.data.max_size() ||
                entry.data.size() > std::numeric_limits<uLong>::max()) {
                error = "归档文件过大"; entries.clear(); return false;
            }
            std::vector<uint8_t> plain(static_cast<size_t>(size));
            uLongf decodedSize = static_cast<uLongf>(size);
            if (uncompress(plain.data(), &decodedSize, entry.data.data(),
                           static_cast<uLong>(entry.data.size())) != Z_OK || decodedSize != size) {
                error = "归档解压失败"; entries.clear(); return false;
            }
            entry.data = std::move(plain);
        } else if (entry.data.size() != size) { error = "归档文件长度不匹配"; entries.clear(); return false; }
        entries.push_back(std::move(entry));
    }
    if (reader.offset != body.size()) { error = "归档包含多余数据"; entries.clear(); return false; }
    return true;
}
}  // namespace dbackup::archive_format



#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <utility>

namespace dbackup::archive_restore {
namespace fs = std::filesystem;
namespace {
class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        if (this != &other) { if (fd_ >= 0) close(fd_); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    int get() const { return fd_; }
private:
    int fd_;
};

bool fail(const std::string &path, std::string &error) {
    error = path + ": " + std::strerror(errno);
    return false;
}

// Resolve each component relative to an already-open directory, never following links.
FileDescriptor directoryAt(int root, const fs::path &path, bool create, std::string &error) {
    FileDescriptor current(dup(root));
    if (current.get() < 0) { fail(path.string(), error); return FileDescriptor(); }
    for (const auto &part : path) {
        if (part == "." || part.empty()) continue;
        int next = openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(current.get(), part.c_str(), 0700) != 0 && errno != EEXIST) {
                fail(path.string(), error); return FileDescriptor();
            }
            next = openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) { fail(path.string(), error); return FileDescriptor(); }
        current = FileDescriptor(next);
    }
    return current;
}

timespec modificationTime(int64_t nanos) {
    timespec value{nanos / 1000000000LL, nanos % 1000000000LL};
    if (value.tv_nsec < 0) { --value.tv_sec; value.tv_nsec += 1000000000; }
    return value;
}

bool metadataFd(int fd, const linux_file::Metadata &metadata, const std::string &path, std::string &error) {
    if (fchown(fd, metadata.uid, metadata.gid) != 0 || fchmod(fd, metadata.mode & 07777) != 0)
        return fail(path, error);
    timespec times[2]{{0, UTIME_OMIT}, modificationTime(metadata.mtimeNs)};
    if (futimens(fd, times) != 0) return fail(path, error);
    return true;
}

bool metadataLink(int parent, const std::string &name, const linux_file::Metadata &metadata,
                  const std::string &path, std::string &error) {
    if (fchownat(parent, name.c_str(), metadata.uid, metadata.gid, AT_SYMLINK_NOFOLLOW) != 0)
        return fail(path, error);
    timespec times[2]{{0, UTIME_OMIT}, modificationTime(metadata.mtimeNs)};
    if (utimensat(parent, name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) return fail(path, error);
    return true;
}

bool writeFile(int parent, const std::string &name, const archive_format::Entry &entry,
               const OperationContext &context, std::string &error) {
    std::string temp;
    FileDescriptor output;
    for (unsigned i = 0; i < 100; ++i) {
        temp = ".dbackup-" + std::to_string(getpid()) + "-" + std::to_string(i) + ".tmp";
        int fd = openat(parent, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) { output = FileDescriptor(fd); break; }
        if (errno != EEXIST) return fail(entry.file.relativePath, error);
    }
    if (output.get() < 0) { error = "无法创建恢复临时文件: " + entry.file.relativePath; return false; }
    size_t offset = 0;
    while (offset < entry.data.size()) {
        if (context.isCancelled()) { error = "操作已取消"; unlinkat(parent, temp.c_str(), 0); return false; }
        ssize_t written = write(output.get(), entry.data.data() + offset, entry.data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { fail(entry.file.relativePath, error); unlinkat(parent, temp.c_str(), 0); return false; }
        offset += static_cast<size_t>(written);
    }
    if (!metadataFd(output.get(), entry.file.metadata, entry.file.relativePath, error) || fsync(output.get()) != 0) {
        if (error.empty()) fail(entry.file.relativePath, error);
        unlinkat(parent, temp.c_str(), 0); return false;
    }
    if (renameat(parent, temp.c_str(), parent, name.c_str()) != 0) {
        fail(entry.file.relativePath, error); unlinkat(parent, temp.c_str(), 0); return false;
    }
    return true;
}
}

bool restore(const std::vector<archive_format::Entry> &entries, const fs::path &destination,
             const OperationContext &context, std::string &error) {
    error.clear();
    std::set<std::string> paths, regularFiles;
    for (const auto &entry : entries) {
        const auto &file = entry.file;
        fs::path path(file.relativePath);
        if (!linux_file::validRelativePath(path) || file.type > 3 || !paths.insert(file.relativePath).second) {
            error = "归档包含重复或不安全的路径: " + file.relativePath; return false;
        }
        if (file.type == 2) {
            fs::path target(file.linkTarget);
            fs::path resolved = (path.parent_path() / target).lexically_normal();
            if (target.empty() || target.is_absolute() || !linux_file::validRelativePath(resolved)) {
                error = "归档包含不安全的符号链接: " + file.relativePath; return false;
            }
        }
        if (file.type == 3 && !regularFiles.count(file.linkTarget)) {
            error = "硬链接目标不是先前的普通文件: " + file.linkTarget; return false;
        }
        if (file.type == 0) regularFiles.insert(file.relativePath);
    }
    if (context.isCancelled()) { error = "操作已取消"; return false; }
    FileDescriptor root(linux_file::openDirectory(destination, true, error));
    if (root.get() < 0) return false;
    uint64_t completed = 0;
    for (const auto &entry : entries) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        const auto &file = entry.file;
        fs::path path(file.relativePath);
        FileDescriptor parent = directoryAt(root.get(), path.parent_path(), true, error);
        if (parent.get() < 0) return false;
        const std::string name = path.filename().string();
        if (file.type == 1) {
            if (mkdirat(parent.get(), name.c_str(), 0700) != 0 && errno != EEXIST) return fail(file.relativePath, error);
            FileDescriptor directory(openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (directory.get() < 0) return fail(file.relativePath, error);
        } else if (file.type == 0) {
            if (!writeFile(parent.get(), name, entry, context, error)) return false;
        } else if (file.type == 2) {
            if (symlinkat(file.linkTarget.c_str(), parent.get(), name.c_str()) != 0) return fail(file.relativePath, error);
            if (!metadataLink(parent.get(), name, file.metadata, file.relativePath, error)) return false;
        } else {
            fs::path target(file.linkTarget);
            FileDescriptor targetParent = directoryAt(root.get(), target.parent_path(), false, error);
            if (targetParent.get() < 0) return false;
            if (linkat(targetParent.get(), target.filename().c_str(), parent.get(), name.c_str(), 0) != 0)
                return fail(file.relativePath, error);
        }
        OperationProgress progress; progress.stage = OperationStage::Restoring;
        progress.currentPath = file.relativePath; progress.completedFiles = ++completed;
        progress.totalFiles = entries.size(); context.report(progress);
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->file.type != 1) continue;
        FileDescriptor directory = directoryAt(root.get(), fs::path(it->file.relativePath), false, error);
        if (directory.get() < 0 || !metadataFd(directory.get(), it->file.metadata, it->file.relativePath, error)) return false;
    }
    OperationProgress done; done.stage = OperationStage::Completed;
    done.completedFiles = completed; done.totalFiles = entries.size(); context.report(done);
    return true;
}
}  // namespace dbackup::archive_restore


#include "core/archive.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

bool ParseFilterOptions(const std::string &spec, FilterOptions &filter, std::string &error) {
    filter = FilterOptions{};
    error.clear();
    auto trim = [](std::string value) {
        const char *spaces = " \t\r\n";
        size_t first = value.find_first_not_of(spaces);
        if (first == std::string::npos) return std::string{};
        size_t last = value.find_last_not_of(spaces);
        return value.substr(first, last - first + 1);
    };
    auto parseSize = [&](const std::string &value, uint64_t &target, const std::string &key) {
        if (value.empty() || value[0] == '-') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        errno = 0;
        char *end = nullptr;
        unsigned long long number = std::strtoull(value.c_str(), &end, 10);
        if (errno == ERANGE || end == value.c_str() || *end != '\0') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        target = static_cast<uint64_t>(number);
        return true;
    };
    auto parseDate = [&](const std::string &value, int64_t &target, const std::string &key, bool endOfDay) {
        std::tm tm = {};
        std::istringstream input(value);
        input >> std::get_time(&tm, "%Y-%m-%d");
        if (input.fail() || input.peek() != std::char_traits<char>::eof()) {
            error = "筛选项 " + key + " 的日期格式应为 YYYY-MM-DD";
            return false;
        }
        const int expectedYear = tm.tm_year;
        const int expectedMonth = tm.tm_mon;
        const int expectedDay = tm.tm_mday;
        time_t seconds = timegm(&tm);
        if (seconds == static_cast<time_t>(-1)) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        std::tm check = {};
        gmtime_r(&seconds, &check);
        if (check.tm_year != expectedYear || check.tm_mon != expectedMonth || check.tm_mday != expectedDay) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        target = static_cast<int64_t>(seconds) * 1000 + (endOfDay ? 86399999LL : 0LL);
        return true;
    };

    std::stringstream stream(spec);
    std::string part;
    while (std::getline(stream, part, ';')) {
        part = trim(part);
        if (part.empty()) continue;
        size_t equal = part.find('=');
        if (equal == std::string::npos) {
            error = "筛选项缺少等号: " + part;
            return false;
        }
        std::string key = Lower(trim(part.substr(0, equal)));
        std::string value = trim(part.substr(equal + 1));
        if (key.empty() || value.empty()) {
            error = "筛选项的名称和值不能为空: " + part;
            return false;
        }
        if (key == "path") filter.pathContains = value;
        else if (key == "name") filter.nameContains = value;
        else if (key == "ext" || key == "extension") {
            std::stringstream extensions(value);
            std::string extension;
            while (std::getline(extensions, extension, ',')) {
                extension = trim(extension);
                if (!extension.empty()) filter.extensions.push_back(extension);
            }
            if (filter.extensions.empty()) {
                error = "筛选项 ext 至少需要一个扩展名";
                return false;
            }
            if (filter.extensions.size() == 1) filter.extension = filter.extensions.front();
        }
        else if (key == "include" || key == "exclude") {
            filter.pathRules.push_back(PathFilterRule{key == "include", value});
        }
        else if (key == "owner" || key == "user") filter.ownerContains = value;
        else if (key == "type") {
            std::string type = Lower(value);
            if (type == "file" || type == "files") filter.type = EntryTypeFilter::FilesOnly;
            else if (type == "dir" || type == "directory" || type == "directories") {
                filter.type = EntryTypeFilter::DirectoriesOnly;
            } else {
                error = "筛选项 type 只支持 file 或 dir";
                return false;
            }
        } else if (key == "minsize") {
            if (!parseSize(value, filter.minSize, key)) return false;
        } else if (key == "maxsize") {
            if (!parseSize(value, filter.maxSize, key)) return false;
        } else if (key == "after") {
            if (!parseDate(value, filter.afterMtimeMs, key, false)) return false;
        } else if (key == "before") {
            if (!parseDate(value, filter.beforeMtimeMs, key, true)) return false;
        } else {
            error = "未知筛选项: " + key;
            return false;
        }
    }
    if (filter.minSize != 0 && filter.maxSize != 0 && filter.minSize > filter.maxSize) {
        error = "minsize 不能大于 maxsize";
        return false;
    }
    if (filter.afterMtimeMs != 0 && filter.beforeMtimeMs != 0 &&
        filter.afterMtimeMs > filter.beforeMtimeMs) {
        error = "after 不能晚于 before";
        return false;
    }
    return true;
}



#include "core/archive.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
using dbackup::archive_format::Entry;
namespace fs = std::filesystem;

bool readSource(const dbackup::archive_scan::Entry &entry, std::vector<uint8_t> &data,
                const OperationContext &context, std::string &error) {
    int fd = open(entry.source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { error = "无法读取文件: " + entry.source.string() + ": " + std::strerror(errno); return false; }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        uint64_t(info.st_dev) != entry.metadata.device || uint64_t(info.st_ino) != entry.metadata.inode ||
        uint64_t(info.st_size) != entry.metadata.size || uint32_t(info.st_mode) != entry.metadata.mode ||
        uint32_t(info.st_uid) != entry.metadata.uid || uint32_t(info.st_gid) != entry.metadata.gid ||
        int64_t(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec != entry.metadata.mtimeNs) {
        close(fd); error = "扫描后文件已改变: " + entry.source.string(); return false;
    }
    char buffer[65536];
    for (;;) {
        if (context.isCancelled()) { close(fd); error = "操作已取消"; return false; }
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = "读取文件失败: " + entry.source.string() + ": " + std::strerror(errno); close(fd); return false; }
        if (count == 0) break;
        data.insert(data.end(), buffer, buffer + count);
    }
    struct stat end{};
    const bool unchanged = fstat(fd, &end) == 0 && end.st_size == info.st_size &&
        end.st_mode == info.st_mode && end.st_uid == info.st_uid && end.st_gid == info.st_gid &&
        end.st_mtim.tv_sec == info.st_mtim.tv_sec && end.st_mtim.tv_nsec == info.st_mtim.tv_nsec &&
        end.st_ctim.tv_sec == info.st_ctim.tv_sec && end.st_ctim.tv_nsec == info.st_ctim.tv_nsec;
    close(fd);
    if (!unchanged) { error = "备份期间文件已改变: " + entry.source.string(); return false; }
    return true;
}

bool readArchive(const std::string &path, std::vector<uint8_t> &data, std::string &error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "无法打开归档: " + path; return false; }
    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) { error = "读取归档失败: " + path; return false; }
    return true;
}

bool writeArchive(const std::string &path, const std::vector<uint8_t> &data,
                  const OperationContext &context, std::string &error) {
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) { error = "无法创建临时归档: " + std::string(std::strerror(errno)); return false; }
    size_t written = 0;
    bool ok = true;
    while (written < data.size()) {
        if (context.isCancelled()) { error = "操作已取消"; ok = false; break; }
        ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = "写入归档失败: " + std::string(std::strerror(errno)); ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (ok && fsync(fd) != 0) { error = "刷新归档失败: " + std::string(std::strerror(errno)); ok = false; }
    if (close(fd) != 0 && ok) { error = "关闭归档失败: " + std::string(std::strerror(errno)); ok = false; }
    if (ok && context.isCancelled()) { error = "操作已取消"; ok = false; }
    if (ok && rename(name.data(), path.c_str()) != 0) { error = "提交归档失败: " + std::string(std::strerror(errno)); ok = false; }
    if (!ok) unlink(name.data());
    return ok;
}

bool decodeFile(const std::string &path, const std::string &password, std::vector<Entry> &entries, std::string &error) {
    std::vector<uint8_t> bytes;
    return readArchive(path, bytes, error) && dbackup::archive_format::decode(bytes, password, entries, error);
}
}

bool ArchiveService::create(const std::string &source, const std::string &destination, std::string &error) {
    return create(source, destination, PackOptions{}, error);
}
bool ArchiveService::create(const std::string &source, const std::string &destination,
                            const PackOptions &options, std::string &error) {
    return create(std::vector<std::string>{source}, destination, options, error, {});
}
bool ArchiveService::create(const std::vector<std::string> &sources, const std::string &destination,
                            const PackOptions &options, std::string &error) {
    return create(sources, destination, options, error, {});
}
bool ArchiveService::create(const std::vector<std::string> &sources, const std::string &destination,
                            const PackOptions &options, std::string &error, const OperationContext &context) {
    error.clear();
    std::vector<dbackup::archive_scan::Entry> scanned;
    OperationProgress progress; progress.stage = OperationStage::Scanning; context.report(progress);
    if (!dbackup::archive_scan::collect(sources, destination, options.filter, scanned, nullptr, context, error)) return false;
    std::vector<Entry> entries;
    progress.stage = OperationStage::Reading; progress.totalFiles = scanned.size();
    for (auto &item : scanned) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        Entry entry; entry.file = std::move(item);
        if (entry.file.type == 0 && !readSource(entry.file, entry.data, context, error)) return false;
        progress.currentPath = entry.file.relativePath; ++progress.completedFiles; context.report(progress);
        entries.push_back(std::move(entry));
    }
    progress.stage = OperationStage::Compressing; context.report(progress);
    std::vector<uint8_t> bytes;
    if (!dbackup::archive_format::encode(entries, options.compressionLevel, options.password, bytes, error) ||
        !writeArchive(destination, bytes, context, error)) return false;
    progress.stage = OperationStage::Completed; context.report(progress); return true;
}
bool ArchiveService::preview(const std::vector<std::string> &sources, const FilterOptions &filter,
                             BackupPreview &preview, std::string &error) {
    std::vector<dbackup::archive_scan::Entry> scanned;
    return dbackup::archive_scan::collect(sources, {}, filter, scanned, &preview, {}, error);
}
bool ArchiveService::inspect(const std::string &file, std::vector<ArchiveEntry> &entries, std::string &error) {
    return inspect(file, entries, error, "");
}
bool ArchiveService::inspect(const std::string &file, std::vector<ArchiveEntry> &entries,
                             std::string &error, const std::string &password) {
    entries.clear();
    std::vector<Entry> decoded;
    if (!decodeFile(file, password, decoded, error)) return false;
    for (const auto &item : decoded) {
        ArchiveEntry entry{};
        entry.type = item.file.type; entry.flags = item.compressed ? 1 : 0; entry.relativePath = item.file.relativePath;
        entry.mode = item.file.metadata.mode; entry.mtimeMs = item.file.metadata.mtimeNs / 1000000;
        entry.owner = item.file.owner; entry.linkTarget = item.file.linkTarget;
        entry.originalSize = item.data.size(); entry.data.assign(item.data.begin(), item.data.end());
        entries.push_back(std::move(entry));
    }
    return true;
}
bool ArchiveService::extract(const std::string &file, const std::string &destination, std::string &error) {
    return extract(file, destination, error, "", {});
}
bool ArchiveService::extract(const std::string &file, const std::string &destination,
                             std::string &error, const std::string &password) {
    return extract(file, destination, error, password, {});
}
bool ArchiveService::extract(const std::string &file, const std::string &destination,
                             std::string &error, const std::string &password, const OperationContext &context) {
    std::vector<Entry> decoded;
    return decodeFile(file, password, decoded, error) && dbackup::archive_restore::restore(decoded, destination, context, error);
}


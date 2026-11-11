#include "core/incremental_chain.h"
#include "core/crypto.h"
#include "core/incremental_archive.h"
#include "core/incremental_manifest.h"
#include "core/incremental_scan.h"
#include "core/linux_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace dbackup::incremental {
namespace {
namespace fs = std::filesystem;
constexpr size_t maxManifest = 64u * 1024u * 1024u + 52;

struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

bool lockDirectory(Fd& lock, int root, int flags, int operation, std::string& error) {
    lock.value = openat(root, ".lock", flags | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info {};
    if (lock.value < 0 || fstat(lock.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        flock(lock.value, operation) != 0) {
        error = "无法锁定增量备份目录";
        return false;
    }
    return true;
}

std::string versionName(const Manifest& version) {
    const std::time_t timestamp = static_cast<std::time_t>(version.createdAtMs / 1000);
    std::tm local{};
    if (!localtime_r(&timestamp, &local)) return {};
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S")
        << std::setfill('0') << std::setw(3) << (version.createdAtMs % 1000)
        << (version.full ? "-full" : "-incremental");
    return out.str();
}

bool parseVersion(const std::string& name, uint64_t& result) {
    size_t start = 0;
    if (name.size() == 21 && name[0] == 'v') start = 1;
    else {
        if (name.size() >= 23 && name[8] == '-' && name[18] == '-' &&
            (name.substr(19) == "full" || name.substr(19) == "incremental")) {
            for (size_t i = 0; i < 18; ++i)
                if (i != 8 && (name[i] < '0' || name[i] > '9')) return false;
            result = 0; // The sequence is read from the manifest.
            return true;
        }
        if (name.size() < 22 || name[8] != '-' || name[15] != '-') return false;
        for (size_t i = 0; i < 15; ++i)
            if (i != 8 && (name[i] < '0' || name[i] > '9')) return false;
        if (name.compare(16, 5, "full-") == 0) start = 21;
        else if (name.compare(16, 12, "incremental-") == 0) start = 28;
        else return false;
        if (start >= name.size()) return false;
    }
    uint64_t value = 0;
    for (size_t i = start; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9' ||
            value > (std::numeric_limits<uint64_t>::max() - (name[i] - '0')) / 10)
            return false;
        value = value * 10 + (name[i] - '0');
    }
    if (!value)
        return false;
    result = value;
    return true;
}

bool readBytes(int directory, const char* name, std::vector<uint8_t>& bytes, std::string& error) {
    Fd fd(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info {};
    if (fd.value < 0 || fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || uint64_t(info.st_size) > maxManifest) {
        error = "无法读取增量备份清单";
        return false;
    }
    std::vector<uint8_t> result(static_cast<size_t>(info.st_size));
    size_t done = 0;
    while (done < result.size()) {
        const ssize_t n = read(fd.value, result.data() + done, result.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            error = "读取增量备份清单失败";
            return false;
        }
        done += static_cast<size_t>(n);
    }
    bytes = std::move(result);
    return true;
}

bool writeBytes(int directory, const char* name, const std::vector<uint8_t>& bytes,
                std::string& error) {
    Fd fd(openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.value < 0) {
        error = "无法创建增量备份清单: " + std::string(std::strerror(errno));
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = write(fd.value, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            error = "写入增量备份清单失败";
            return false;
        }
        done += static_cast<size_t>(n);
    }
    if (fsync(fd.value) != 0) {
        error = "刷新增量备份清单失败";
        return false;
    }
    return true;
}

bool containsPath(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c)
        if (c == child.end() || *p != *c)
            return false;
    return true;
}

bool safeDestination(const std::string& directory, const std::vector<std::string>& sources,
                     std::string& error) {
    std::error_code ec;
    const auto destination = fs::canonical(directory, ec);
    if (ec) {
        error = "无法解析增量备份目录: " + ec.message();
        return false;
    }
    for (const auto& source : sources) {
        const auto canonical = fs::canonical(source, ec);
        if (ec) {
            error = "无法解析备份来源: " + source + ": " + ec.message();
            return false;
        }
        if (containsPath(canonical, destination)) {
            error = "增量备份目录不能位于备份来源中";
            return false;
        }
    }
    return true;
}

bool loadHistory(int root, const std::string& password, std::vector<Manifest>& history,
                 std::string& error) {
    Fd copy(openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (copy.value < 0) {
        error = "无法读取增量备份目录";
        return false;
    }
    DIR* raw = fdopendir(copy.value);
    if (!raw) {
        error = "无法读取增量备份目录";
        return false;
    }
    copy.value = -1;
    std::vector<std::pair<uint64_t, std::string>> versions;
    errno = 0;
    while (auto* item = readdir(raw)) {
        uint64_t sequence;
        if (parseVersion(item->d_name, sequence))
            versions.emplace_back(sequence, item->d_name);
        errno = 0;
    }
    const int readError = errno;
    closedir(raw);
    if (readError) {
        error = "枚举增量备份版本失败";
        return false;
    }
    std::sort(versions.begin(), versions.end());
    std::vector<Manifest> found;
    for (const auto& [sequence, name] : versions) {
        Fd version(openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        std::vector<uint8_t> bytes;
        Manifest manifest;
        if (version.value < 0 || !readBytes(version.value, "manifest", bytes, error)) {
            if (error.empty()) error = "无法打开增量备份版本: " + name;
            return false;
        }
        const bool decoded = password.empty() ? decodeManifest(bytes, manifest, error)
                                              : decodeProtectedManifest(bytes, password, manifest, error);
        if (!decoded || (sequence != 0 && manifest.sequence != sequence)) {
            if (error.empty()) error = "增量清单版本不匹配";
            return false;
        }
        manifest.directoryName = name;
        found.push_back(std::move(manifest));
    }
    std::sort(found.begin(), found.end(), [](const Manifest& a, const Manifest& b) {
        return a.sequence < b.sequence;
    });
    if (!found.empty() && !validateChain(found, error))
        return false;
    history = std::move(found);
    return true;
}

std::string randomName() {
    std::vector<uint8_t> bytes;
    if (!crypto::randomBytes(bytes, 16))
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string name = ".pending-";
    for (auto b : bytes) {
        name.push_back(hex[b >> 4]);
        name.push_back(hex[b & 15]);
    }
    return name;
}

bool archiveName(int version, std::string& name, std::string& error) {
    name.clear();
    for (const char* candidate : {"archive.tar", "archive.abk", "archive"}) {
        struct stat info {};
        if (fstatat(version, candidate, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode) || !name.empty()) {
                error = "增量版本包含无效或重复的归档文件";
                return false;
            }
            name = candidate;
        } else if (errno != ENOENT) {
            error = "无法检查增量归档文件";
            return false;
        }
    }
    if (name.empty()) {
        error = "增量版本缺少归档文件";
        return false;
    }
    return true;
}

bool verifyHistory(int root, const std::string& password, const std::vector<Manifest>& history,
                   const OperationContext& context, std::string& error, size_t first = 0) {
    for (size_t i = first; i < history.size(); ++i) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const auto& version = history[i];
        Fd folder(openat(root, version.directoryName.c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (folder.value < 0) {
            error = "无法打开增量备份版本: " + version.directoryName;
            return false;
        }
        std::string name;
        if (!archiveName(folder.value, name, error)) return false;
        const std::string archive = "/proc/self/fd/" + std::to_string(folder.value) + "/" + name;
        const Manifest* previous = version.full ? nullptr : &history[i - 1];
        if (!verifyArchive(archive, password, version, previous, context, error)) {
            error = "版本 " + std::to_string(version.sequence) + " 校验失败: " + error;
            return false;
        }
    }
    return true;
}

bool fileChanges(const Manifest* previous, const Manifest& current,
                 std::vector<std::string>& added, std::vector<std::string>& modified,
                 std::vector<std::string>& deleted, std::string& error) {
    Changes changes;
    const std::vector<FileState> empty;
    if (!compare(previous ? previous->files : empty, current.files, changes, error)) return false;
    std::map<std::string, uint8_t> oldTypes, newTypes;
    if (previous) for (const auto& file : previous->files) oldTypes[file.path] = file.type;
    for (const auto& file : current.files) newTypes[file.path] = file.type;
    for (const auto& path : changes.changed) {
        auto old = oldTypes.find(path);
        const bool oldFile = old != oldTypes.end() && old->second != 1;
        const bool newFile = newTypes.at(path) != 1;
        if (!oldFile && newFile) added.push_back(path);
        else if (oldFile && !newFile) deleted.push_back(path);
        else if (oldFile && newFile) modified.push_back(path);
    }
    for (const auto& path : changes.deleted)
        if (oldTypes.at(path) != 1) deleted.push_back(path);
    return true;
}

bool writePublicSummary(int dir, const Manifest& manifest, const CreatedVersion& changes,
                        std::string& error) {
    std::ostringstream out;
    out << "DBACKUP-PUBLIC-1\n" << manifest.sequence << ' ' << manifest.createdAtMs << ' '
        << manifest.full << '\n';
    const auto write = [&out](char kind, const std::vector<std::string>& paths) {
        for (const auto& path : paths) out << kind << ' ' << std::quoted(path) << '\n';
    };
    write('A', changes.added); write('M', changes.modified); write('D', changes.deleted);
    const auto data = out.str();
    if (data.size() > maxManifest) { error = "备份变化摘要过大"; return false; }
    return writeBytes(dir, "changes", std::vector<uint8_t>(data.begin(), data.end()), error);
}

bool readPublicSummary(int dir, VersionInfo& info, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readBytes(dir, "changes", bytes, error)) return false;
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string header;
    std::getline(in, header);
    int full;
    if (header != "DBACKUP-PUBLIC-1" || !(in >> info.sequence >> info.createdAtMs >> full) ||
        !info.sequence || (full != 0 && full != 1)) {
        error = "备份变化摘要格式无效"; return false;
    }
    info.full = full;
    char kind;
    std::string path;
    size_t count = 0;
    while (in >> kind) {
        if (++count > 1000000 || !(in >> std::quoted(path)) || path.find('\0') != std::string::npos ||
            !linux_file::validRelativePath(path)) {
            error = "备份变化摘要包含无效路径"; return false;
        }
        if (kind == 'A') info.added.push_back(path);
        else if (kind == 'M') info.modified.push_back(path);
        else if (kind == 'D') info.deleted.push_back(path);
        else { error = "备份变化摘要包含无效类型"; return false; }
    }
    info.changesAvailable = true;
    return true;
}

// This operates only inside a private restore staging directory. Directory
// components are opened without following symlinks.
int openRestoreDirectory(int root, const fs::path& path, std::string& error) {
    int current = dup(root);
    if (current < 0) {
        error = "无法打开恢复暂存目录";
        return -1;
    }
    for (const auto& part : path) {
        const std::string name = part.string();
        if (name.empty() || name == ".")
            continue;
        struct stat info {};
        if (name == ".." || fstatat(current, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(info.st_mode) ||
            fchmodat(current, name.c_str(), (info.st_mode & 07777) | 0700, 0) != 0) {
            error = "无法访问恢复暂存目录: " + path.string();
            close(current);
            return -1;
        }
        int next = openat(current, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(current);
        if (next < 0) {
            error = "无法打开恢复暂存目录: " + path.string();
            return -1;
        }
        current = next;
    }
    return current;
}

bool removeEntry(int parent, const std::string& name, const OperationContext& context,
                 std::string& error) {
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    struct stat info {};
    if (fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        error = "恢复暂存区缺少预期文件: " + name;
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (unlinkat(parent, name.c_str(), 0) == 0)
            return true;
        error = "无法移除恢复暂存条目: " + name;
        return false;
    }
    if (fchmodat(parent, name.c_str(), (info.st_mode & 07777) | 0700, 0) != 0) {
        error = "无法访问恢复暂存目录: " + name;
        return false;
    }
    Fd directory(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    Fd duplicate(directory.value >= 0 ? dup(directory.value) : -1);
    if (duplicate.value < 0) {
        error = "无法打开恢复暂存目录: " + name;
        return false;
    }
    DIR* raw = fdopendir(duplicate.value);
    if (!raw) {
        error = "无法枚举恢复暂存目录: " + name;
        return false;
    }
    duplicate.value = -1;
    bool ok = true;
    errno = 0;
    while (auto* child = readdir(raw)) {
        const std::string childName(child->d_name);
        if (childName != "." && childName != ".." &&
            !removeEntry(directory.value, childName, context, error)) {
            ok = false;
            break;
        }
        errno = 0;
    }
    const int readError = errno;
    closedir(raw);
    if (!ok)
        return false;
    if (readError || unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = "无法移除恢复暂存目录: " + name;
        return false;
    }
    return true;
}

bool removeRelative(int root, const std::string& path, const OperationContext& context,
                    std::string& error) {
    const fs::path relative(path);
    if (!linux_file::validRelativePath(relative)) {
        error = "恢复清单包含不安全的路径";
        return false;
    }
    Fd parent(openRestoreDirectory(root, relative.parent_path(), error));
    return parent.value >= 0 && removeEntry(parent.value, relative.filename().string(), context, error);
}

bool applyRemovals(int root, const Manifest& before, const Manifest& after,
                   const OperationContext& context, std::string& error) {
    Changes changes;
    if (!compare(before.files, after.files, changes, error))
        return false;
    std::map<std::string, uint8_t> oldTypes, newTypes;
    for (const auto& f : before.files) oldTypes.emplace(f.path, f.type);
    for (const auto& f : after.files) newTypes.emplace(f.path, f.type);
    std::set<std::string> remove(changes.deleted.begin(), changes.deleted.end());
    for (const auto& path : changes.changed) {
        auto old = oldTypes.find(path);
        if (old != oldTypes.end() && !(old->second == 1 && newTypes.at(path) == 1))
            remove.insert(path);
    }
    std::vector<std::string> ordered(remove.begin(), remove.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        const auto da = std::count(a.begin(), a.end(), '/');
        const auto db = std::count(b.begin(), b.end(), '/');
        return da != db ? da > db : a < b;
    });
    for (const auto& path : ordered)
        if (!removeRelative(root, path, context, error))
            return false;
    return true;
}

bool copyArchive(int version, int stage, const OperationContext& context, std::string& error) {
    std::string name;
    if (!archiveName(version, name, error)) return false;
    Fd input(openat(version, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    Fd output(openat(stage, "archive", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    struct stat info {};
    if (input.value < 0 || output.value < 0 || fstat(input.value, &info) != 0 ||
        !S_ISREG(info.st_mode)) {
        error = "无法暂存增量归档";
        return false;
    }
    std::array<uint8_t, 128 * 1024> buffer{};
    for (;;) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const ssize_t n = read(input.value, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { error = "读取增量归档失败"; return false; }
        if (!n) break;
        size_t done = 0;
        while (done < static_cast<size_t>(n)) {
            const ssize_t written = write(output.value, buffer.data() + done, n - done);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = "暂存增量归档失败"; return false; }
            done += static_cast<size_t>(written);
        }
    }
    struct stat after {};
    if (fstat(input.value, &after) != 0 || info.st_size != after.st_size ||
        info.st_mtim.tv_sec != after.st_mtim.tv_sec || info.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        info.st_ctim.tv_sec != after.st_ctim.tv_sec || info.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        fsync(output.value) != 0) {
        error = "暂存时归档发生变化或写入失败";
        return false;
    }
    return true;
}

bool restoreDirectoryMetadata(int root, const Manifest& manifest, std::string& error) {
    std::vector<const FileState*> dirs;
    for (const auto& file : manifest.files)
        if (file.type == 1) dirs.push_back(&file);
    std::sort(dirs.begin(), dirs.end(), [](const auto* a, const auto* b) {
        return std::count(a->path.begin(), a->path.end(), '/') >
               std::count(b->path.begin(), b->path.end(), '/');
    });
    for (const auto* dir : dirs) {
        Fd fd(openRestoreDirectory(root, fs::path(dir->path), error));
        if (fd.value < 0)
            return false;
        timespec times[2]{{0, UTIME_OMIT}, {dir->mtimeNs / 1000000000LL, dir->mtimeNs % 1000000000LL}};
        if (times[1].tv_nsec < 0) { --times[1].tv_sec; times[1].tv_nsec += 1000000000; }
        if (fchown(fd.value, dir->uid, dir->gid) != 0 || fchmod(fd.value, dir->mode & 07777) != 0 ||
            futimens(fd.value, times) != 0) {
            error = "无法完整恢复目录元数据: " + dir->path;
            return false;
        }
    }
    return true;
}
} // namespace

bool createVersion(const std::string& directory, const std::vector<std::string>& sources,
                   const PackOptions& options, const OperationContext& context,
                   CreatedVersion& created, std::string& error) {
    error.clear();
    if ((options.format != PackFormat::Tar && options.format != PackFormat::Custom) || sources.empty()) {
        error = "增量备份需要来源和有效的归档格式";
        return false;
    }
    Fd root(linux_file::openDirectory(directory, true, error));
    if (root.value < 0 || !safeDestination(directory, sources, error))
        return false;
    Fd lock;
    if (!lockDirectory(lock, root.value, O_CREAT | O_RDWR, LOCK_EX, error))
        return false;
    std::vector<Manifest> history;
    if (!loadHistory(root.value, options.password, history, error))
        return false;
    size_t activeStart = history.empty() ? 0 : history.size() - 1;
    while (activeStart > 0 && !history[activeStart].full)
        --activeStart;
    if (!verifyHistory(root.value, options.password, history, context, error, activeStart))
        return false;
    if (!history.empty() && history.back().sequence == std::numeric_limits<uint64_t>::max()) {
        error = "增量备份版本号已用尽";
        return false;
    }
    Manifest next;
    next.sequence = history.empty() ? 1 : history.back().sequence + 1;
    next.createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    size_t sinceFull = 0;
    for (auto it = history.rbegin(); it != history.rend() && !it->full; ++it)
        ++sinceFull;
    next.full = history.empty() || sinceFull >= 6;
    next.previous = next.full ? 0 : history.back().sequence;
    std::vector<archive_scan::Entry> scanned;
    if (!scanForArchive(sources, options.filter, context, scanned, next.files, error))
        return false;
    std::vector<std::string> selected;
    if (next.full) {
        for (const auto& file : next.files)
            selected.push_back(file.path);
    } else {
        Changes changes;
        if (!compare(history.back().files, next.files, changes, error))
            return false;
        // A standalone delta cannot safely replace one name of a hard-link
        // group without potentially breaking unchanged names. Start a new
        // full backup when any path in such a group changes.
        std::set<std::string> linkedPaths;
        for (const auto* files : {&history.back().files, &next.files}) {
            for (const auto& file : *files) {
                if (file.type == 3) {
                    linkedPaths.insert(file.path);
                    linkedPaths.insert(file.linkTarget);
                }
            }
        }
        const auto touchesLink = [&linkedPaths](const std::vector<std::string>& paths) {
            return std::any_of(paths.begin(), paths.end(), [&linkedPaths](const auto& path) {
                return linkedPaths.count(path) != 0;
            });
        };
        if (touchesLink(changes.changed) || touchesLink(changes.deleted)) {
            next.full = true;
            next.previous = 0;
            for (const auto& file : next.files)
                selected.push_back(file.path);
        } else {
            selected = std::move(changes.changed);
            next.deleted = std::move(changes.deleted);
        }
    }
    CreatedVersion result;
    result.sequence = next.sequence;
    result.full = next.full;
    if (!fileChanges(history.empty() ? nullptr : &history.back(), next,
                     result.added, result.modified, result.deleted, error)) return false;
    const std::string stagingName = randomName();
    if (stagingName.empty() || mkdirat(root.value, stagingName.c_str(), 0700) != 0) {
        error = "无法创建增量备份暂存目录";
        return false;
    }
    struct Staging {
        int parent;
        std::string name;
        bool committed = false;
        ~Staging() {
            if (committed) return;
            Fd directory(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (directory.value >= 0) {
                unlinkat(directory.value, "archive", 0);
                unlinkat(directory.value, "archive.tar", 0);
                unlinkat(directory.value, "archive.abk", 0);
                unlinkat(directory.value, "manifest", 0);
                unlinkat(directory.value, "changes", 0);
            }
            unlinkat(parent, name.c_str(), AT_REMOVEDIR);
        }
    } staging{root.value, stagingName};
    Fd stage(openat(root.value, stagingName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (stage.value < 0) {
        error = "无法打开增量备份暂存目录";
        return false;
    }
    const bool plainTar = options.format == PackFormat::Tar && options.password.empty() &&
        (options.compression == Compression::None ||
         (options.compression == Compression::Deflate && options.compressionLevel == 0));
    const std::string archive = "/proc/self/fd/" + std::to_string(stage.value) +
                                (plainTar ? "/archive.tar" : "/archive.abk");
    OperationContext local = context;
    local.noReplace = true;
    local.onProgress = [&context](const OperationProgress& progress) {
        if (progress.stage != OperationStage::Completed)
            context.report(progress);
    };
    if (!writeSelectedArchive(scanned, selected, archive, options, local, error) ||
        !verifyArchive(archive, options.password, next, next.full ? nullptr : &history.back(),
                       context, error))
        return false;
    std::vector<uint8_t> encoded;
    const bool encodedOk = options.password.empty() ? encodeManifest(next, encoded, error)
        : encodeProtectedManifest(next, options.password, encoded, error);
    if (!encodedOk || !writeBytes(stage.value, "manifest", encoded, error) ||
        !writePublicSummary(stage.value, next, result, error) ||
        fsync(stage.value) != 0) {
        if (error.empty()) error = "刷新增量备份版本失败";
        return false;
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    auto naming = next;
    auto finalName = versionName(naming);
    if (finalName.empty()) { error = "无法生成备份版本名称"; return false; }
    while (syscall(SYS_renameat2, root.value, stagingName.c_str(), root.value,
                   finalName.c_str(), 1u) != 0) {
        if (errno != EEXIST || naming.createdAtMs == std::numeric_limits<int64_t>::max()) {
            error = "提交增量备份版本失败: " + std::string(std::strerror(errno));
            return false;
        }
        ++naming.createdAtMs;
        finalName = versionName(naming);
        if (finalName.empty()) { error = "无法生成备份版本名称"; return false; }
    }
    staging.committed = true;
    if (fsync(root.value) != 0) {
        error = "版本已提交，但无法确认目录已同步";
        return false;
    }
    created = std::move(result);
    OperationProgress progress;
    progress.stage = OperationStage::Completed;
    context.report(progress);
    return true;
}

bool listVersions(const std::string& directory, const std::string& password,
                  const OperationContext& context, std::vector<VersionInfo>& versions,
                  std::string& error) {
    error.clear();
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    Fd root(linux_file::openDirectory(directory, false, error));
    if (root.value < 0)
        return false;
    Fd lock;
    if (!lockDirectory(lock, root.value, O_RDONLY, LOCK_SH, error))
        return false;
    std::vector<Manifest> history;
    if (!loadHistory(root.value, password, history, error))
        return false;
    if (history.empty()) {
        error = "没有可读取的增量备份版本";
        return false;
    }
    if (!verifyHistory(root.value, password, history, context, error))
        return false;
    std::vector<VersionInfo> result;
    result.reserve(history.size());
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& version = history[i];
        VersionInfo info{version.sequence, version.full, version.createdAtMs, version.files.size(),
                         version.deleted.size()};
        info.directoryName = version.directoryName;
        info.changesAvailable = i > 0 || version.sequence == 1;
        if (info.changesAvailable && !fileChanges(i ? &history[i - 1] : nullptr, version,
                                                   info.added, info.modified, info.deleted, error))
            return false;
        result.push_back(std::move(info));
    }
    versions = std::move(result);
    return true;
}

bool listPublicVersions(const std::string& directory, const OperationContext& context,
                        std::vector<VersionInfo>& versions, std::string& error) {
    error.clear();
    Fd root(linux_file::openDirectory(directory, false, error));
    if (root.value < 0) return false;
    Fd lock;
    if (!lockDirectory(lock, root.value, O_RDONLY, LOCK_SH, error))
        return false;
    Fd scan(openat(root.value, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (scan.value < 0) { error = "无法读取备份目录"; return false; }
    DIR* raw = fdopendir(scan.value);
    if (!raw) { error = "无法读取备份目录"; return false; }
    scan.value = -1;
    std::vector<VersionInfo> found;
    errno = 0;
    while (auto* entry = readdir(raw)) {
        if (context.isCancelled()) { closedir(raw); error = "操作已取消"; return false; }
        uint64_t sequence = 0;
        const std::string name = entry->d_name;
        if (!parseVersion(name, sequence)) { errno = 0; continue; }
        Fd dir(openat(root.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dir.value < 0) { closedir(raw); error = "无法打开备份版本"; return false; }
        VersionInfo version;
        version.directoryName = name;
        version.sequence = sequence;
        struct stat fileInfo {};
        if (fstatat(dir.value, "changes", &fileInfo, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(fileInfo.st_mode) || !readPublicSummary(dir.value, version, error)) {
                closedir(raw); return false;
            }
        } else if (errno != ENOENT) { closedir(raw); error = "无法读取备份变化摘要"; return false; }
        else {
            std::tm parsed{};
            parsed.tm_isdst = -1;
            std::istringstream timestamp(name.substr(0, 15));
            if (timestamp >> std::get_time(&parsed, "%Y%m%d-%H%M%S")) {
                const auto seconds = std::mktime(&parsed);
                if (seconds != static_cast<std::time_t>(-1))
                    version.createdAtMs = static_cast<int64_t>(seconds) * 1000;
            }
            version.full = name.find("-full") != std::string::npos;
        }
        found.push_back(std::move(version));
        errno = 0;
    }
    const int readError = errno;
    closedir(raw);
    if (readError) { error = "枚举备份目录失败"; return false; }
    std::sort(found.begin(), found.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return a.createdAtMs != b.createdAtMs ? a.createdAtMs < b.createdAtMs
                                              : a.directoryName < b.directoryName;
    });
    versions = std::move(found);
    return true;
}

bool restoreVersion(const std::string& directory, uint64_t sequence, const std::string& password,
                    const std::string& destination, const OperationContext& context,
                    std::string& error) {
    error.clear();
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    std::error_code ec;
    const fs::path absolute = fs::absolute(destination, ec).lexically_normal();
    const std::string target = absolute.filename().string();
    if (ec || destination.empty() || target.empty() || target == "." || target == "..") {
        error = "恢复目标必须是新的目录路径";
        return false;
    }
    Fd parent(linux_file::openDirectory(absolute.parent_path(), false, error));
    Fd root(linux_file::openDirectory(directory, false, error));
    if (parent.value < 0 || root.value < 0)
        return false;
    struct stat existing {};
    if (fstatat(parent.value, target.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        error = "恢复目标已存在或无法访问；请选择新的目录";
        return false;
    }
    Fd lock;
    if (!lockDirectory(lock, root.value, O_RDONLY, LOCK_SH, error))
        return false;
    std::vector<Manifest> history;
    if (!loadHistory(root.value, password, history, error))
        return false;
    const auto selected = std::find_if(history.begin(), history.end(), [sequence](const auto& m) {
        return m.sequence == sequence;
    });
    if (selected == history.end()) {
        error = "找不到指定的增量备份版本";
        return false;
    }
    const size_t last = static_cast<size_t>(selected - history.begin());
    size_t first = last;
    while (first > 0 && !history[first].full)
        --first;
    const std::string name = randomName();
    if (name.empty() || mkdirat(parent.value, name.c_str(), 0700) != 0) {
        error = "无法创建恢复暂存目录";
        return false;
    }
    struct Cleanup {
        int parent;
        std::string name;
        ~Cleanup() {
            std::string ignored;
            removeEntry(parent, name, {}, ignored);
        }
    } cleanup{parent.value, name};
    Fd stage(openat(parent.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (stage.value < 0 || mkdirat(stage.value, "data", 0700) != 0) {
        error = "无法准备恢复暂存目录";
        return false;
    }
    Fd data(openat(stage.value, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (data.value < 0) {
        error = "无法打开恢复暂存目录";
        return false;
    }
    const std::string archive = "/proc/self/fd/" + std::to_string(stage.value) + "/archive";
    for (size_t i = first; i <= last; ++i) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        Fd version(openat(root.value, history[i].directoryName.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (version.value < 0 || !copyArchive(version.value, stage.value, context, error)) {
            if (error.empty()) error = "无法打开增量备份版本";
            return false;
        }
        const Manifest* previous = i == first ? nullptr : &history[i - 1];
        if (!verifyArchive(archive, password, history[i], previous, context, error))
            return false;
        if (previous && !applyRemovals(data.value, *previous, history[i], context, error))
            return false;
        OperationContext local = context;
        local.suppressSocketNotice = true;
        local.onProgress = [&context](const OperationProgress& progress) {
            if (progress.stage != OperationStage::Completed)
                context.report(progress);
        };
        if (!extractArchiveToFd(archive, data.value, password, local, error))
            return false;
        if (!error.empty()) {
            error = "恢复未完整完成: " + error;
            return false;
        }
        if (unlinkat(stage.value, "archive", 0) != 0) {
            error = "无法清理恢复暂存归档";
            return false;
        }
    }
    if (!restoreDirectoryMetadata(data.value, history[last], error) || context.isCancelled()) {
        if (error.empty()) error = "操作已取消";
        return false;
    }
    if (fsync(data.value) != 0 ||
        syscall(SYS_renameat2, stage.value, "data", parent.value, target.c_str(), 1u) != 0) {
        error = "无法提交恢复目录: " + std::string(std::strerror(errno));
        return false;
    }
    if (fsync(parent.value) != 0) {
        error = "恢复目录已创建，但无法确认目录已同步";
        return false;
    }
    OperationProgress progress;
    progress.stage = OperationStage::Completed;
    context.report(progress);
    for (const auto& file : history[last].files) {
        if (file.type == 7) {
            error = "已恢复套接字节点；它不包含原服务或连接，可能占用服务路径。";
            if (context.warnings)
                context.warnings->push_back(error);
            break;
        }
    }
    return true;
}

bool pruneVersions(const std::string& directory, const std::string& password, size_t keepLatest,
                   const OperationContext& context, size_t& removed, std::string& error,
                   const PackOptions* packOptions) {
    error.clear();
    removed = 0;
    if (!keepLatest) {
        error = "保留数量必须大于零";
        return false;
    }
    Fd root(linux_file::openDirectory(directory, false, error));
    if (root.value < 0)
        return false;
    Fd lock;
    if (!lockDirectory(lock, root.value, O_RDWR, LOCK_EX, error))
        return false;
    std::vector<Manifest> history;
    if (!loadHistory(root.value, password, history, error))
        return false;
    if (history.empty())
        return true;
    if (history.size() > keepLatest && !history[history.size() - keepLatest].full) {
        const size_t boundary = history.size() - keepLatest;
        const auto original = history[boundary];
        const std::string restoreName = randomName();
        const std::string stageName = randomName();
        if (restoreName.empty() || stageName.empty()) {
            error = "无法创建整理备份的临时名称";
            return false;
        }
        struct Cleanup {
            int root;
            std::string name;
            bool active = true;
            ~Cleanup() { if (active) { std::string ignored; removeEntry(root, name, {}, ignored); } }
        } restored{root.value, restoreName}, staged{root.value, stageName};
        // Restore takes a shared lock itself. Recheck the complete history after
        // reacquiring the exclusive lock before publishing anything.
        if (flock(lock.value, LOCK_UN) != 0) {
            error = "无法释放备份锁";
            return false;
        }
        OperationContext local = context;
        local.onProgress = {};
        std::string restoreError;
        const bool restoredOk = restoreVersion(directory, original.sequence, password,
                                               (fs::path(directory) / restoreName).string(),
                                               local, restoreError);
        if (flock(lock.value, LOCK_EX) != 0) {
            error = "无法重新锁定增量备份目录";
            return false;
        }
        if (!restoredOk) {
            error = "整理保留版本时恢复失败: " + restoreError;
            return false;
        }
        std::vector<Manifest> current;
        if (!loadHistory(root.value, password, current, error) || current.size() != history.size()) {
            if (error.empty()) error = "整理期间备份链已改变，请重试";
            return false;
        }
        for (size_t i = 0; i < history.size(); ++i) {
            std::vector<uint8_t> oldBytes, newBytes;
            if (!encodeManifest(history[i], oldBytes, error) ||
                !encodeManifest(current[i], newBytes, error) || oldBytes != newBytes) {
                if (error.empty()) error = "整理期间备份链已改变，请重试";
                return false;
            }
        }
        if (!verifyHistory(root.value, password, current, context, error)) return false;
        std::vector<std::string> sources;
        for (const auto& file : original.files)
            if (file.path.find('/') == std::string::npos)
                sources.push_back((fs::path(directory) / restoreName / file.path).string());
        std::vector<archive_scan::Entry> entries;
        std::vector<FileState> states;
        if (!sources.empty() && !scanForArchive(sources, {}, local, entries, states, error))
            return false;
        if (mkdirat(root.value, stageName.c_str(), 0700) != 0) {
            error = "无法创建整理后的归档目录";
            return false;
        }
        Fd stage(openat(root.value, stageName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (stage.value < 0) { error = "无法打开整理后的归档目录"; return false; }
        PackOptions options = packOptions ? *packOptions : PackOptions{};
        options.password = password;
        options.filter = {};
        const bool plainTar = options.format == PackFormat::Tar && password.empty() &&
            (options.compression == Compression::None ||
             (options.compression == Compression::Deflate && options.compressionLevel == 0));
        const std::string archive = "/proc/self/fd/" + std::to_string(stage.value) +
                                    (plainTar ? "/archive.tar" : "/archive.abk");
        std::vector<std::string> selected;
        for (const auto& file : original.files) selected.push_back(file.path);
        Manifest replacement = original;
        replacement.full = true;
        replacement.previous = 0;
        replacement.deleted.clear();
        local.noReplace = true;
        if (!writeSelectedArchive(entries, selected, archive, options, local, error) ||
            !verifyArchive(archive, password, replacement, nullptr, context, error))
            return false;
        std::vector<uint8_t> encoded;
        const bool encodedOk = password.empty() ? encodeManifest(replacement, encoded, error)
            : encodeProtectedManifest(replacement, password, encoded, error);
        Fd oldVersion(openat(root.value, original.directoryName.c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat summaryInfo {};
        if (oldVersion.value < 0) { error = "无法打开待整理的备份版本"; return false; }
        if (fstatat(oldVersion.value, "changes", &summaryInfo, AT_SYMLINK_NOFOLLOW) == 0) {
            VersionInfo summary;
            if (!S_ISREG(summaryInfo.st_mode) || !readPublicSummary(oldVersion.value, summary, error)) return false;
            CreatedVersion saved;
            saved.added = std::move(summary.added);
            saved.modified = std::move(summary.modified);
            saved.deleted = std::move(summary.deleted);
            if (!writePublicSummary(stage.value, replacement, saved, error)) return false;
        } else if (errno != ENOENT) { error = "无法读取备份变化摘要"; return false; }
        if (!encodedOk || !writeBytes(stage.value, "manifest", encoded, error) ||
            fsync(stage.value) != 0) {
            if (error.empty()) error = "无法同步整理后的备份";
            return false;
        }
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        const auto target = original.directoryName;
        if (syscall(SYS_renameat2, root.value, stageName.c_str(), root.value,
                    target.c_str(), 2u) != 0) {
            error = "无法原子替换整理后的备份版本: " + std::string(std::strerror(errno));
            return false;
        }
        // The exchanged directory contains the old version until the swap is durable.
        staged.active = false;
        if (fsync(root.value) != 0) {
            error = "已整理备份但无法确认目录同步，旧版本仍保存在 " + stageName;
            return false;
        }
        staged.active = true;
        if (target[0] != 'v') {
            auto naming = replacement;
            auto updatedName = versionName(naming);
            while (!updatedName.empty() &&
                   syscall(SYS_renameat2, root.value, target.c_str(), root.value,
                           updatedName.c_str(), 1u) != 0) {
                if (errno != EEXIST || naming.createdAtMs == std::numeric_limits<int64_t>::max()) {
                    error = "备份已整理，但无法更新版本目录名称";
                    return false;
                }
                ++naming.createdAtMs;
                updatedName = versionName(naming);
            }
            if (updatedName.empty()) { error = "无法生成备份版本名称"; return false; }
            replacement.directoryName = updatedName;
            if (fsync(root.value) != 0) {
                error = "版本目录已重命名，但无法确认目录已同步";
                return false;
            }
        }
        history[boundary] = std::move(replacement);
    }
    size_t prefix = 0;
    if (!removablePrefix(history, keepLatest, prefix, error))
        return false;
    if (prefix && !verifyHistory(root.value, password, history, context, error))
        return false;
    size_t start = 0;
    while (start < prefix) {
        size_t end = start + 1;
        while (end < prefix && !history[end].full)
            ++end;
        // Remove newest deltas first and the full version last. Renaming each
        // version out of the visible namespace makes interrupted deletion safe.
        for (size_t position = end; position > start; --position) {
            if (context.isCancelled()) {
                error = "清理已取消";
                return false;
            }
            const std::string source = history[position - 1].directoryName;
            const std::string quarantine = randomName();
            if (quarantine.empty() ||
                syscall(SYS_renameat2, root.value, source.c_str(), root.value,
                        quarantine.c_str(), 1u) != 0) {
                error = "无法移除旧备份版本: " + source;
                return false;
            }
            ++removed;
            if (fsync(root.value) != 0) {
                error = "旧版本已移出列表，但无法确认目录已同步: " + quarantine;
                return false;
            }
            if (!removeEntry(root.value, quarantine, {}, error)) {
                error = "旧版本已移出列表，暂存目录清理失败: " + quarantine + ": " + error;
                return false;
            }
            if (fsync(root.value) != 0) {
                error = "旧版本已清理，但无法确认目录已同步";
                return false;
            }
        }
        start = end;
    }
    return true;
}
} // namespace dbackup::incremental

#include "core/archive.h"
#include "core/archive_internal.h"
#include "core/backup_policy.h"
#include "core/crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace dbackup::archive_restore {
namespace fs = std::filesystem;
namespace {
class FileDescriptor {
  public:
    // 接管文件描述符，析构时负责关闭。
    explicit FileDescriptor(int fd = -1) : fd_(fd) {
    }
    // 对象销毁时关闭仍由当前实例持有的描述符。
    ~FileDescriptor() {
        if (fd_ >= 0)
            close(fd_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    // 关闭当前描述符，允许通过移动语义转移所有权。
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {
    }
    // 先关闭当前描述符，再接管另一个对象的描述符。
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    // 返回当前持有的原始描述符，不转移所有权。
    int get() const {
        return fd_;
    }

  private:
    int fd_;
};

// 将系统调用失败转换为带目标路径的错误提示。
bool fail(const std::string& path, std::string& error) {
    error = path + ": " + std::strerror(errno);
    return false;
}
// 判断错误是否可作为恢复警告继续处理其余条目。
bool recoverableError(int value) {
    return value == EACCES || value == EPERM || value == ENOTSUP || value == EOPNOTSUPP;
}
// 记录权限或文件系统不支持造成的元数据不完整情况。
bool metadataFailure(const std::string& path, std::string& warnings) {
    if (!recoverableError(errno))
        return fail(path, warnings);
    if (!warnings.empty())
        warnings += "\n";
    warnings += path + ": 元数据未完整恢复: " + std::strerror(errno);
    return true;
}

// 从已打开目录开始逐级解析路径组件，创建缺失目录且不跟随符号链接。
FileDescriptor directoryAt(int root, const fs::path& path, bool create, std::string& error) {
    FileDescriptor current(dup(root));
    if (current.get() < 0) {
        fail(path.string(), error);
        return FileDescriptor();
    }
    for (const auto& part : path) {
        if (part == "." || part.empty())
            continue;
        int next =
            openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(current.get(), part.c_str(), 0700) != 0 && errno != EEXIST) {
                fail(path.string(), error);
                return FileDescriptor();
            }
            next = openat(current.get(), part.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            fail(path.string(), error);
            return FileDescriptor();
        }
        current = FileDescriptor(next);
    }
    return current;
}

// 将纳秒时间戳转换为 timespec，并规范化负时间的纳秒部分。
timespec modificationTime(int64_t nanos) {
    timespec value{nanos / 1000000000LL, nanos % 1000000000LL};
    if (value.tv_nsec < 0) {
        --value.tv_sec;
        value.tv_nsec += 1000000000;
    }
    return value;
}

// 比较实际恢复元数据与归档记录，并报告目标文件系统丢失的字段。
void verifyMetadata(const struct stat& actual, const linux_file::Metadata& expected,
                    const std::string& path, bool symlink, std::string& warnings) {
    const int64_t actualMtime =
        int64_t(actual.st_mtim.tv_sec) * 1000000000LL + actual.st_mtim.tv_nsec;
    std::string fields;
    if (actual.st_uid != expected.uid || actual.st_gid != expected.gid)
        fields += "属主/属组 ";
    if (!symlink && (actual.st_mode & 07777) != (expected.mode & 07777))
        fields += "权限 ";
    if (actualMtime != expected.mtimeNs)
        fields += "修改时间 ";
    if (!fields.empty()) {
        if (!warnings.empty())
            warnings += "\n";
        warnings += path + ": 目标文件系统未保留 " + fields;
    }
}

// 对已打开的普通文件应用属主、权限和修改时间并回读验证。
bool metadataFd(int fd, const linux_file::Metadata& metadata, const std::string& path,
                std::string& error) {
    if (fchown(fd, metadata.uid, metadata.gid) != 0 && !metadataFailure(path, error))
        return false;
    if (fchmod(fd, metadata.mode & 07777) != 0 && !metadataFailure(path, error))
        return false;
    timespec times[2]{{0, UTIME_OMIT}, modificationTime(metadata.mtimeNs)};
    if (futimens(fd, times) != 0 && !metadataFailure(path, error))
        return false;
    struct stat actual {};
    if (fstat(fd, &actual) != 0)
        return fail(path, error);
    verifyMetadata(actual, metadata, path, false, error);
    return true;
}

// 对符号链接应用不跟随链接的属主和时间属性并回读验证。
bool metadataLink(int parent, const std::string& name, const linux_file::Metadata& metadata,
                  const std::string& path, std::string& error) {
    if (fchownat(parent, name.c_str(), metadata.uid, metadata.gid, AT_SYMLINK_NOFOLLOW) != 0 &&
        !metadataFailure(path, error))
        return false;
    timespec times[2]{{0, UTIME_OMIT}, modificationTime(metadata.mtimeNs)};
    if (utimensat(parent, name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0 &&
        !metadataFailure(path, error))
        return false;
    struct stat actual {};
    if (fstatat(parent, name.c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0)
        return fail(path, error);
    verifyMetadata(actual, metadata, path, true, error);
    return true;
}

// 在支持的内核上修改符号链接本身的权限，不跟随链接目标。
int chmodNoFollow(int parent, const char* name, mode_t mode) {
#if defined(SYS_fchmodat2)
    if (syscall(SYS_fchmodat2, parent, name, mode, AT_SYMLINK_NOFOLLOW) == 0)
        return 0;
    if (errno != ENOSYS)
        return -1;
#elif defined(__x86_64__) || defined(__aarch64__)
    // Ubuntu 22.04 headers predate fchmodat2, but newer Linux kernels provide it.
    if (syscall(452, parent, name, mode, AT_SYMLINK_NOFOLLOW) == 0)
        return 0;
    if (errno != ENOSYS)
        return -1;
#endif
    return fchmodat(parent, name, mode, AT_SYMLINK_NOFOLLOW);
}

// 先写入同目录临时文件并恢复元数据，再原子替换正式目标文件。
bool writeFile(int parent, const std::string& name, const archive_format::Entry& entry,
               const OperationContext& context, std::string& error,
               const ContentWriter& contentWriter) {
    std::string temp;
    FileDescriptor output;
    for (unsigned i = 0; i < 100; ++i) {
        temp = ".dbackup-" + std::to_string(getpid()) + "-" + std::to_string(i) + ".tmp";
        int fd = openat(parent, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
        if (fd >= 0) {
            output = FileDescriptor(fd);
            break;
        }
        if (errno != EEXIST)
            return fail(entry.file.relativePath, error);
    }
    if (output.get() < 0) {
        error = "无法创建恢复临时文件: " + entry.file.relativePath;
        return false;
    }
    struct CleanupTemporary {
        int parent;
        const std::string& name;
        bool committed = false;
        ~CleanupTemporary() { if (!committed) unlinkat(parent, name.c_str(), 0); }
    } cleanup{parent, temp};
    if (contentWriter && !contentWriter(entry, output.get(), context, error)) {
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    size_t offset = 0;
    while (!contentWriter && offset < entry.data.size()) {
        if (context.isCancelled()) {
            error = "操作已取消";
            unlinkat(parent, temp.c_str(), 0);
            return false;
        }
        ssize_t written =
            write(output.get(), entry.data.data() + offset, entry.data.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            fail(entry.file.relativePath, error);
            unlinkat(parent, temp.c_str(), 0);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    if (!metadataFd(output.get(), entry.file.metadata, entry.file.relativePath, error)) {
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    if (fsync(output.get()) != 0) {
        fail(entry.file.relativePath, error);
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    if (renameat(parent, temp.c_str(), parent, name.c_str()) != 0) {
        fail(entry.file.relativePath, error);
        unlinkat(parent, temp.c_str(), 0);
        return false;
    }
    cleanup.committed = true;
    return true;
}
} // namespace

static bool restoreImpl(const std::vector<archive_format::Entry>& entries,
                        const fs::path& destination, int destinationFd,
                        const OperationContext& context, std::string& error,
                        const ContentWriter& contentWriter);

// 安全恢复条目，阻断路径逃逸并逐项报告权限、设备和元数据限制。
bool restore(const std::vector<archive_format::Entry>& entries, const fs::path& destination,
             const OperationContext& context, std::string& error) {
    return restore(entries, destination, context, error, {});
}

bool restore(const std::vector<archive_format::Entry>& entries, const fs::path& destination,
             const OperationContext& context, std::string& error,
             const ContentWriter& contentWriter) {
    return restoreImpl(entries, destination, -1, context, error, contentWriter);
}

bool restoreAt(const std::vector<archive_format::Entry>& entries, int destinationFd,
               const OperationContext& context, std::string& error,
               const ContentWriter& contentWriter) {
    if (destinationFd < 0) {
        error = "无效的恢复目录";
        return false;
    }
    return restoreImpl(entries, {}, destinationFd, context, error, contentWriter);
}

static bool restoreImpl(const std::vector<archive_format::Entry>& entries,
                        const fs::path& destination, int destinationFd,
                        const OperationContext& context, std::string& error,
                        const ContentWriter& contentWriter) {
    error.clear();
    std::set<std::string> paths, regularFiles;
    for (const auto& entry : entries) {
        const auto& file = entry.file;
        fs::path path(file.relativePath);
        if (!linux_file::validRelativePath(path) || file.type > 7 ||
            !paths.insert(file.relativePath).second) {
            error = "归档包含重复或不安全的路径: " + file.relativePath;
            return false;
        }
        if (file.type == 2) {
            fs::path target(file.linkTarget);
            fs::path resolved = (path.parent_path() / target).lexically_normal();
            if (target.empty() || target.is_absolute() ||
                !linux_file::validRelativePath(resolved)) {
                error = "归档包含不安全的符号链接: " + file.relativePath;
                return false;
            }
        }
        if (file.type == 3 && !regularFiles.count(file.linkTarget)) {
            error = "硬链接目标不是先前的普通文件: " + file.linkTarget;
            return false;
        }
        if (file.type == 0)
            regularFiles.insert(file.relativePath);
    }
    if (context.isCancelled()) {
        error = "操作已取消";
        return false;
    }
    FileDescriptor root(destinationFd >= 0 ? dup(destinationFd)
                                          : linux_file::openDirectory(destination, true, error));
    if (root.get() < 0)
        return false;
    uint64_t completed = 0;
    std::vector<std::string> warnings;
    std::set<std::string> skippedDirectories, skippedPaths;
    auto skip = [&](const std::string& path, const std::string& reason, bool directory) {
        warnings.push_back(path + ": " + reason);
        skippedPaths.insert(path);
        if (directory)
            skippedDirectories.insert(path);
    };
    auto parentSkipped = [&](const fs::path& path) {
        for (const auto& parent : skippedDirectories) {
            auto relative = path.lexically_relative(parent);
            if (!relative.empty() && *relative.begin() != "..")
                return true;
        }
        return false;
    };
    for (const auto& entry : entries) {
        if (context.isCancelled()) {
            error = "操作已取消";
            return false;
        }
        const auto& file = entry.file;
        fs::path path(file.relativePath);
        if (parentSkipped(path)) {
            skip(file.relativePath, "上级目录未恢复", file.type == 1);
            continue;
        }
        if ((file.type == 5 || file.type == 6) && !context.allowDeviceNodes) {
            skip(file.relativePath, "设备节点恢复未启用", false);
            continue;
        }
        if (file.type == 3 && skippedPaths.count(file.linkTarget)) {
            skip(file.relativePath, "硬链接目标未恢复", false);
            continue;
        }
        std::string detail;
        errno = 0;
        FileDescriptor parent = directoryAt(root.get(), path.parent_path(), true, detail);
        if (parent.get() < 0) {
            if (recoverableError(errno)) {
                skip(file.relativePath, detail, file.type == 1);
                continue;
            }
            error = detail;
            return false;
        }
        const std::string name = path.filename().string();
        bool ok = true;
        if (file.type == 1) {
            if (mkdirat(parent.get(), name.c_str(), 0700) != 0 && errno != EEXIST)
                ok = fail(file.relativePath, detail);
            if (ok) {
                FileDescriptor directory(openat(parent.get(), name.c_str(),
                                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (directory.get() < 0)
                    ok = fail(file.relativePath, detail);
            }
        } else if (file.type == 0) {
            ok = writeFile(parent.get(), name, entry, context, detail, contentWriter);
        } else if (file.type == 2) {
            if (symlinkat(file.linkTarget.c_str(), parent.get(), name.c_str()) != 0)
                ok = fail(file.relativePath, detail);
            else
                ok = metadataLink(parent.get(), name, file.metadata, file.relativePath, detail);
        } else if (file.type == 3) {
            fs::path target(file.linkTarget);
            FileDescriptor targetParent =
                directoryAt(root.get(), target.parent_path(), false, detail);
            if (targetParent.get() < 0)
                ok = false;
            else {
                struct stat targetInfo {};
                if (fstatat(targetParent.get(), target.filename().c_str(), &targetInfo,
                            AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISREG(targetInfo.st_mode)) {
                    error = "硬链接目标不是普通文件: " + file.linkTarget;
                    return false;
                }
                if (linkat(targetParent.get(), target.filename().c_str(), parent.get(),
                           name.c_str(), 0) != 0)
                    ok = fail(file.relativePath, detail);
            }
        } else {
            const mode_t type =
                file.type == 4 ? S_IFIFO
                               : (file.type == 5 ? S_IFCHR : (file.type == 6 ? S_IFBLK : S_IFSOCK));
            dev_t device = makedev(file.metadata.deviceMajor, file.metadata.deviceMinor);
            if ((file.type == 5 || file.type == 6) &&
                (major(device) != file.metadata.deviceMajor ||
                 minor(device) != file.metadata.deviceMinor)) {
                error = "设备号无效: " + file.relativePath;
                return false;
            }
            if (mknodat(parent.get(), name.c_str(), type | (file.metadata.mode & 07777), device) !=
                0)
                ok = fail(file.relativePath, detail);
            else {
                if (fchownat(parent.get(), name.c_str(), file.metadata.uid, file.metadata.gid,
                             AT_SYMLINK_NOFOLLOW) != 0)
                    ok = metadataFailure(file.relativePath, detail);
                if (ok &&
                    chmodNoFollow(parent.get(), name.c_str(), file.metadata.mode & 07777) != 0)
                    ok = metadataFailure(file.relativePath, detail);
                timespec times[2]{{0, UTIME_OMIT}, modificationTime(file.metadata.mtimeNs)};
                if (ok && utimensat(parent.get(), name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0)
                    ok = metadataFailure(file.relativePath, detail);
                struct stat actual {};
                if (ok && fstatat(parent.get(), name.c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0)
                    ok = fail(file.relativePath, detail);
                if (ok)
                    verifyMetadata(actual, file.metadata, file.relativePath, false, detail);
                if (ok && file.type == 7 && !context.suppressSocketNotice) {
                    if (!detail.empty())
                        detail += "\\n";
                    detail +=
                        file.relativePath + ": 套接字节点不包含原服务或连接，可能占用服务路径";
                }
            }
        }
        if (!ok) {
            const int cause = errno;
            if (!context.isCancelled() &&
                (recoverableError(cause) || (file.type == 7 && cause == EEXIST))) {
                skip(file.relativePath, detail, file.type == 1);
                continue;
            }
            error = detail;
            return false;
        }
        if (!detail.empty())
            warnings.push_back(detail);
        OperationProgress progress;
        progress.stage = OperationStage::Restoring;
        progress.currentPath = file.relativePath;
        progress.completedFiles = ++completed;
        progress.totalFiles = entries.size();
        context.report(progress);
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->file.type != 1 || skippedPaths.count(it->file.relativePath))
            continue;
        std::string detail;
        FileDescriptor directory =
            directoryAt(root.get(), fs::path(it->file.relativePath), false, detail);
        if (directory.get() < 0 ||
            !metadataFd(directory.get(), it->file.metadata, it->file.relativePath, detail)) {
            if (recoverableError(errno)) {
                warnings.push_back(detail);
                continue;
            }
            error = detail;
            return false;
        }
        if (!detail.empty())
            warnings.push_back(detail);
    }
    error.clear();
    for (const auto& warning : warnings) {
        if (!error.empty())
            error += "\\n";
        error += warning;
        context.warn(warning);
    }
    OperationProgress done;
    done.stage = OperationStage::Completed;
    done.completedFiles = completed;
    done.totalFiles = entries.size();
    context.report(done);
    return true;
}
} // namespace dbackup::archive_restore


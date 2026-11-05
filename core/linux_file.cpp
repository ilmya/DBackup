#include "core/linux_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace dbackup::linux_file {
namespace fs = std::filesystem;
namespace {
bool fail(const fs::path &path, std::string &error) {
    error = path.string() + ": " + std::strerror(errno);
    return false;
}
}

bool readMetadata(const fs::path &path, Metadata &metadata, std::string &error) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) return fail(path, error);
    metadata.mode = info.st_mode;
    metadata.uid = info.st_uid;
    metadata.gid = info.st_gid;
    metadata.mtimeNs = int64_t(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec;
    metadata.device = info.st_dev;
    metadata.inode = info.st_ino;
    metadata.size = info.st_size;
    metadata.links = info.st_nlink;
    metadata.deviceMajor = (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)) ? major(info.st_rdev) : 0;
    metadata.deviceMinor = (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)) ? minor(info.st_rdev) : 0;
    return true;
}

bool validRelativePath(const fs::path &path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto &part : path) {
        if (part == "." || part == ".." || part.empty()) return false;
    }
    return true;
}

int openDirectory(const fs::path &path, bool create, std::string &error) {
    if (path.empty()) { error = "目录路径为空"; return -1; }
    int fd = open(path.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { fail(path, error); return -1; }
    for (const auto &part : path.relative_path()) {
        if (part == ".") continue;
        if (part == "..") { close(fd); error = "目录路径包含 .."; return -1; }
        int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, part.c_str(), 0777) != 0 && errno != EEXIST) {
                int saved = errno; close(fd); errno = saved; fail(path, error); return -1;
            }
            next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) { int saved = errno; close(fd); errno = saved; fail(path, error); return -1; }
        close(fd); fd = next;
    }
    return fd;
}

bool ensureDirectories(const fs::path &path, std::string &error) {
    int fd = openDirectory(path, true, error);
    if (fd < 0) return false;
    close(fd);
    return true;
}

}  // namespace dbackup::linux_file

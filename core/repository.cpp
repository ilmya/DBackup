#include "core/repository.h"
#include "core/archive_internal.h"
#include "core/crypto.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using dbackup::archive_format::Entry;
constexpr char configMagic[] = "DBRL0001";
constexpr char chunkMagic[] = "DBCL0001";

class RepositoryLock {
public:
    RepositoryLock(const fs::path &root, bool exclusive, std::string &error) {
        int parent = dbackup::linux_file::openDirectory(root, false, error);
        if (parent < 0) return;
        fd_ = openat(parent, "config.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        if (fd_ < 0 || flock(fd_, exclusive ? LOCK_EX : LOCK_SH) != 0) {
            error = "无法锁定仓库";
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
        }
    }
    ~RepositoryLock() { if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); } }
    bool valid() const { return fd_ >= 0; }
private:
    int fd_ = -1;
};

std::string hex(const std::vector<uint8_t> &data) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (auto byte : data) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}
bool validHash(const std::string &hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool validId(const std::string &id) {
    return id.size() == 30 && id[13] == '-' &&
        std::all_of(id.begin(), id.begin() + 13, [](char c) { return c >= '0' && c <= '9'; }) &&
        std::all_of(id.begin() + 14, id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool keyFor(const std::string &password, const std::vector<uint8_t> &salt, std::vector<uint8_t> &key) {
    key.resize(32);
    return dbackup::crypto::pbkdf2Sha256(password, salt, dbackup::crypto::kDefaultPbkdf2Iterations, key);
}
bool readFile(const fs::path &path, std::vector<uint8_t> &data, std::string &error) {
    int parent = dbackup::linux_file::openDirectory(path.parent_path(), false, error);
    if (parent < 0) return false;
    int fd = openat(parent, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (fd < 0) { error = "无法读取文件: " + path.string(); return false; }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { close(fd); error = "文件类型无效: " + path.string(); return false; }
    data.clear();
    uint8_t buffer[65536];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); error = "读取文件失败: " + path.string(); return false; }
        if (!count) break;
        data.insert(data.end(), buffer, buffer + count);
    }
    close(fd);
    return true;
}
bool writeAtomic(const fs::path &path, const std::vector<uint8_t> &data, bool replace, std::string &error) {
    int parent = dbackup::linux_file::openDirectory(path.parent_path(), false, error);
    if (parent < 0) return false;
    std::string temporary;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        std::vector<uint8_t> random;
        if (!dbackup::crypto::randomBytes(random, 8)) break;
        temporary = ".dbackup-" + hex(random) + ".tmp";
        fd = openat(parent, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { close(parent); error = "无法创建临时文件: " + path.string(); return false; }
    size_t offset = 0; bool ok = true;
    while (offset < data.size()) {
        ssize_t count = write(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += size_t(count);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok) {
        if (replace) ok = renameat(parent, temporary.c_str(), parent, path.filename().c_str()) == 0;
        else ok = linkat(parent, temporary.c_str(), parent, path.filename().c_str(), 0) == 0;
        if (!replace && ok) unlinkat(parent, temporary.c_str(), 0);
    }
    if (!ok) { unlinkat(parent, temporary.c_str(), 0); error = "无法提交文件: " + path.string(); }
    close(parent);
    return ok;
}

bool seal(const char *magic, const std::vector<uint8_t> &key, const std::vector<uint8_t> &plain,
          std::vector<uint8_t> &stored, std::string &error) {
    std::vector<uint8_t> nonce, tag, cipher;
    if (!dbackup::crypto::aes256Gcm(true, plain, key, nonce, tag, cipher)) { error = "加密失败"; return false; }
    stored.assign(magic, magic + 8);
    stored.insert(stored.end(), nonce.begin(), nonce.end());
    stored.insert(stored.end(), tag.begin(), tag.end());
    stored.insert(stored.end(), cipher.begin(), cipher.end());
    return true;
}
bool unseal(const char *magic, const std::vector<uint8_t> &key, const std::vector<uint8_t> &stored,
            std::vector<uint8_t> &plain, std::string &error) {
    if (stored.size() < 36 || std::memcmp(stored.data(), magic, 8)) { error = "文件格式无效"; return false; }
    std::vector<uint8_t> nonce(stored.begin() + 8, stored.begin() + 20);
    std::vector<uint8_t> tag(stored.begin() + 20, stored.begin() + 36);
    std::vector<uint8_t> cipher(stored.begin() + 36, stored.end());
    if (!dbackup::crypto::aes256Gcm(false, cipher, key, nonce, tag, plain)) { error = "密码错误或数据已损坏"; return false; }
    return true;
}
fs::path chunkPath(const fs::path &root, const std::string &hash) { return root / "chunks" / hash.substr(0, 2) / (hash + ".blk"); }
bool readChunk(const fs::path &root, const std::string &hash, const std::vector<uint8_t> &key,
               std::vector<uint8_t> &plain, std::string &error) {
    if (!validHash(hash)) { error = "数据块标识无效"; return false; }
    std::vector<uint8_t> stored, digest;
    if (!readFile(chunkPath(root, hash), stored, error) || !unseal(chunkMagic, key, stored, plain, error) ||
        !dbackup::crypto::sha256(plain.data(), plain.size(), digest) || hex(digest) != hash) {
        if (error.empty()) error = "数据块哈希不匹配: " + hash;
        return false;
    }
    return true;
}
bool manifests(const fs::path &root, std::vector<std::string> &ids, std::string &error) {
    ids.clear(); std::error_code ec;
    fs::directory_iterator it(root / "snapshots", ec), end;
    if (ec) { error = "无法读取快照目录: " + ec.message(); return false; }
    while (it != end) {
        if (it->path().extension() == ".manifest") {
            const auto id = it->path().stem().string();
            if (!validId(id) || !it->is_regular_file(ec) || it->is_symlink(ec)) { error = "快照文件无效"; return false; }
            ids.push_back(id);
        }
        it.increment(ec);
        if (ec) { error = "读取快照目录失败: " + ec.message(); return false; }
    }
    std::sort(ids.begin(), ids.end(), std::greater<std::string>());
    return true;
}
bool loadManifest(const fs::path &root, const std::string &id, const std::string &password,
                  std::vector<Entry> &entries, std::string &error) {
    if (!validId(id)) { error = "快照标识无效"; return false; }
    std::vector<uint8_t> bytes;
    return readFile(root / "snapshots" / (id + ".manifest"), bytes, error) &&
           dbackup::archive_format::decode(bytes, password, entries, error);
}
bool chunkRefs(const Entry &entry, std::vector<std::string> &hashes, std::string &error) {
    hashes.clear();
    if (entry.file.type != 0) return entry.data.empty();
    if (entry.data.size() % 64) { error = "快照块列表无效"; return false; }
    for (size_t offset = 0; offset < entry.data.size(); offset += 64) {
        std::string hash(entry.data.begin() + offset, entry.data.begin() + offset + 64);
        if (!validHash(hash)) { error = "快照块标识无效"; return false; }
        hashes.push_back(hash);
    }
    return true;
}
}

LocalRepository::LocalRepository(std::string root) : root_(std::move(root)) {}

bool LocalRepository::initialize(const std::string &password, std::string &error) {
    error.clear();
    if (password.empty() || root_.empty()) { error = "仓库路径和密码不能为空"; return false; }
    fs::path root(root_);
    std::error_code ec;
    if (fs::exists(root, ec)) {
        int rootFd = dbackup::linux_file::openDirectory(root, false, error);
        if (rootFd < 0) return false;
        close(rootFd);
    }
    if (ec) { error = "无法读取仓库目录: " + ec.message(); return false; }
    if (fs::exists(root / "config.bin", ec)) {
        std::vector<uint8_t> config;
        if (!readFile(root / "config.bin", config, error) || config.size() < 24 ||
            std::memcmp(config.data(), configMagic, 8)) { error = "仓库配置无效"; return false; }
        std::vector<uint8_t> salt(config.begin() + 8, config.begin() + 24), key, plain;
        std::vector<uint8_t> sealed(config.begin() + 24, config.end());
        if (!keyFor(password, salt, key) || !unseal(configMagic, key, sealed, plain, error) ||
            plain != std::vector<uint8_t>({'D','B','a','c','k','u','p'})) {
            error = "仓库密码错误或配置损坏"; return false;
        }
        int chunksFd = dbackup::linux_file::openDirectory(root / "chunks", false, error);
        if (chunksFd < 0) return false;
        close(chunksFd);
        int snapshotsFd = dbackup::linux_file::openDirectory(root / "snapshots", false, error);
        if (snapshotsFd < 0) return false;
        close(snapshotsFd);
        salt_ = std::move(salt); activePassword_ = password; return true;
    }
    if (ec) { error = "无法读取仓库目录: " + ec.message(); return false; }
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) { error = "仓库目录不为空且缺少有效配置，已保留原有数据"; return false; }
    if (ec || !dbackup::linux_file::ensureDirectories(root, error) ||
        !dbackup::linux_file::ensureDirectories(root / "chunks", error) ||
        !dbackup::linux_file::ensureDirectories(root / "snapshots", error)) return false;
    std::vector<uint8_t> salt, key, sealed;
    const std::vector<uint8_t> verifier{'D','B','a','c','k','u','p'};
    if (!dbackup::crypto::randomBytes(salt, 16) || !keyFor(password, salt, key) ||
        !seal(configMagic, key, verifier, sealed, error)) return false;
    std::vector<uint8_t> config(configMagic, configMagic + 8);
    config.insert(config.end(), salt.begin(), salt.end());
    config.insert(config.end(), sealed.begin(), sealed.end());
    if (!writeAtomic(root / "config.bin", config, false, error)) return false;
    salt_ = std::move(salt); activePassword_ = password; return true;
}

bool LocalRepository::createSnapshot(const SnapshotOptions &options, SnapshotInfo &snapshot,
                                     std::string &error, const OperationContext &context) {
    error.clear(); snapshot = {};
    if (options.chunkSize == 0 || options.chunkSize > 64u * 1024u * 1024u) { error = "数据块大小无效"; return false; }
    if (!initialize(options.password, error)) return false;
    RepositoryLock lock(root_, true, error);
    if (!lock.valid()) return false;
    fs::path root = fs::absolute(root_).lexically_normal();
    for (const auto &source : options.sources) {
        fs::path path = fs::absolute(source).lexically_normal();
        auto inside = [](const fs::path &child, const fs::path &parent) {
            auto relative = child.lexically_relative(parent);
            return !relative.empty() && *relative.begin() != "..";
        };
        if (path == root || inside(root, path) || inside(path, root)) { error = "备份来源与仓库不能互相包含"; return false; }
    }
    std::vector<dbackup::archive_scan::Entry> scanned;
    if (!dbackup::archive_scan::collect(options.sources, {}, options.filter, scanned, nullptr, context, error, true)) return false;
    std::vector<uint8_t> key;
    if (!keyFor(options.password, salt_, key)) { error = "密钥派生失败"; return false; }
    std::vector<Entry> entries;
    snapshot.createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<std::string> existingIds;
    if (!manifests(root, existingIds, error)) return false;
    if (!existingIds.empty()) snapshot.createdAtMs = std::max<int64_t>(snapshot.createdAtMs, std::stoll(existingIds.front().substr(0, 13)) + 1);
    std::vector<uint8_t> random;
    if (!dbackup::crypto::randomBytes(random, 8)) { error = "无法生成快照标识"; return false; }
    snapshot.id = std::to_string(snapshot.createdAtMs) + "-" + hex(random);
    for (auto &item : scanned) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        Entry entry; entry.file = std::move(item);
        if (entry.file.type == 0) {
            int fd = open(entry.file.source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { error = "无法读取来源: " + entry.file.source.string(); return false; }
            struct stat before{};
            if (fstat(fd, &before) || !S_ISREG(before.st_mode) || uint64_t(before.st_dev) != entry.file.metadata.device ||
                uint64_t(before.st_ino) != entry.file.metadata.inode || uint64_t(before.st_size) != entry.file.metadata.size ||
                uint32_t(before.st_mode) != entry.file.metadata.mode || uint32_t(before.st_uid) != entry.file.metadata.uid ||
                uint32_t(before.st_gid) != entry.file.metadata.gid ||
                int64_t(before.st_mtim.tv_sec) * 1000000000LL + before.st_mtim.tv_nsec != entry.file.metadata.mtimeNs) {
                close(fd); error = "扫描后文件已改变"; return false;
            }
            std::vector<uint8_t> buffer(options.chunkSize);
            for (;;) {
                if (context.isCancelled()) { close(fd); error = "操作已取消"; return false; }
                size_t length = 0;
                while (length < buffer.size()) {
                    ssize_t n = read(fd, buffer.data() + length, buffer.size() - length);
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0) { close(fd); error = "读取来源失败"; return false; }
                    if (!n) break;
                    length += size_t(n);
                }
                if (!length) break;
                std::vector<uint8_t> plain(buffer.begin(), buffer.begin() + length), digest;
                if (!dbackup::crypto::sha256(plain.data(), plain.size(), digest)) { close(fd); error = "哈希失败"; return false; }
                std::string hash = hex(digest); fs::path path = chunkPath(root, hash);
                std::error_code ec;
                if (fs::exists(path, ec)) {
                    std::vector<uint8_t> verified;
                    if (!readChunk(root, hash, key, verified, error) || verified != plain) { close(fd); return false; }
                } else {
                    if (ec || !dbackup::linux_file::ensureDirectories(path.parent_path(), error)) { close(fd); return false; }
                    std::vector<uint8_t> stored;
                    if (!seal(chunkMagic, key, plain, stored, error) || !writeAtomic(path, stored, false, error)) { close(fd); return false; }
                    snapshot.storedBytes += stored.size();
                }
                entry.data.insert(entry.data.end(), hash.begin(), hash.end());
                snapshot.logicalBytes += length;
            }
            struct stat after{};
            bool unchanged = fstat(fd, &after) == 0 && before.st_size == after.st_size &&
                before.st_mode == after.st_mode && before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
                before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
            close(fd);
            if (!unchanged) { error = "备份期间文件已改变"; return false; }
            ++snapshot.fileCount;
        }
        entries.push_back(std::move(entry));
    }
    if (context.isCancelled()) { error = "操作已取消"; return false; }
    std::vector<uint8_t> manifest;
    if (!dbackup::archive_format::encode(entries, 6, options.password, manifest, error) ||
        !writeAtomic(root / "snapshots" / (snapshot.id + ".manifest"), manifest, false, error)) return false;
    return true;
}

bool LocalRepository::listSnapshots(std::vector<SnapshotInfo> &snapshots, std::string &error) {
    snapshots.clear(); error.clear();
    RepositoryLock lock(root_, false, error);
    if (!lock.valid()) return false;
    std::vector<uint8_t> config;
    fs::path root(root_);
    if (!readFile(root / "config.bin", config, error) || config.size() < 24 ||
        std::memcmp(config.data(), configMagic, 8)) { error = "仓库配置无效"; return false; }
    std::vector<std::string> ids;
    if (!manifests(root, ids, error)) return false;
    for (const auto &id : ids) {
        SnapshotInfo snapshot;
        snapshot.id = id;
        snapshot.createdAtMs = std::stoll(id.substr(0, 13));
        if (!activePassword_.empty()) {
            std::vector<Entry> entries;
            if (!loadManifest(root, id, activePassword_, entries, error)) return false;
            for (const auto &entry : entries) {
                if (entry.file.type == 0) {
                    ++snapshot.fileCount;
                    std::vector<std::string> refs;
                    if (!chunkRefs(entry, refs, error)) return false;
                    std::vector<uint8_t> key, plain;
                    if (!keyFor(activePassword_, salt_, key)) return false;
                    for (const auto &hash : refs) {
                        if (!readChunk(root, hash, key, plain, error)) return false;
                        snapshot.logicalBytes += plain.size();
                    }
                }
            }
        }
        snapshots.push_back(std::move(snapshot));
    }
    return true;
}

bool LocalRepository::restoreSnapshot(const std::string &id, const std::string &destination,
                                      const std::string &password, std::string &error,
                                      const OperationContext &context) {
    if (!validId(id)) { error = "快照标识无效"; return false; }
    if (!fs::exists(fs::path(root_) / "config.bin")) { error = "仓库不存在或配置缺失"; return false; }
    if (!initialize(password, error)) return false;
    RepositoryLock lock(root_, false, error);
    if (!lock.valid()) return false;
    fs::path root(root_);
    std::vector<Entry> entries;
    if (!loadManifest(root, id, password, entries, error)) return false;
    std::vector<uint8_t> key;
    if (!keyFor(password, salt_, key)) { error = "密钥派生失败"; return false; }
    for (auto &entry : entries) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        if (entry.file.type != 0) continue;
        std::vector<std::string> refs;
        if (!chunkRefs(entry, refs, error)) return false;
        entry.data.clear();
        for (const auto &hash : refs) {
            if (context.isCancelled()) { error = "操作已取消"; return false; }
            std::vector<uint8_t> plain;
            if (!readChunk(root, hash, key, plain, error)) return false;
            entry.data.insert(entry.data.end(), plain.begin(), plain.end());
        }
    }
    return dbackup::archive_restore::restore(entries, destination, context, error);
}

bool LocalRepository::deleteSnapshot(const std::string &id, std::string &error) {
    if (!validId(id)) { error = "快照标识无效"; return false; }
    RepositoryLock lock(root_, true, error);
    if (!lock.valid()) return false;
    int fd = dbackup::linux_file::openDirectory(fs::path(root_) / "snapshots", false, error);
    if (fd < 0) return false;
    int result = unlinkat(fd, (id + ".manifest").c_str(), 0);
    int saved = errno; close(fd);
    if (result != 0) { error = "快照不存在或无法删除: " + std::string(std::strerror(saved)); return false; }
    return true;
}

bool LocalRepository::prune(size_t keep, std::string &error) {
    if (activePassword_.empty() || salt_.empty()) { error = "清理仓库前必须验证密码"; return false; }
    RepositoryLock lock(root_, true, error);
    if (!lock.valid()) return false;
    fs::path root(root_);
    std::vector<std::string> ids;
    if (!manifests(root, ids, error)) return false;
    std::set<std::string> live;
    std::vector<uint8_t> key;
    if (!keyFor(activePassword_, salt_, key)) { error = "密钥派生失败"; return false; }
    for (size_t i = 0; i < std::min(keep, ids.size()); ++i) {
        std::vector<Entry> entries;
        if (!loadManifest(root, ids[i], activePassword_, entries, error)) return false;
        for (const auto &entry : entries) {
            std::vector<std::string> refs;
            if (!chunkRefs(entry, refs, error)) return false;
            for (const auto &hash : refs) {
                std::vector<uint8_t> plain;
                if (!readChunk(root, hash, key, plain, error)) return false;
                live.insert(hash);
            }
        }
    }
    int snapshotsFd = dbackup::linux_file::openDirectory(root / "snapshots", false, error);
    if (snapshotsFd < 0) return false;
    for (size_t i = keep; i < ids.size(); ++i) {
        if (unlinkat(snapshotsFd, (ids[i] + ".manifest").c_str(), 0) != 0) {
            close(snapshotsFd); error = "无法删除快照: " + ids[i]; return false;
        }
    }
    close(snapshotsFd);
    std::error_code ec;
    fs::recursive_directory_iterator it(root / "chunks", fs::directory_options::none, ec), end;
    if (ec) { error = "无法读取数据块目录: " + ec.message(); return false; }
    while (it != end) {
        fs::path path = it->path();
        if (it->is_regular_file(ec) && !it->is_symlink(ec) && path.extension() == ".blk") {
            std::string hash = path.stem().string();
            if (validHash(hash) && !live.count(hash) && !fs::remove(path, ec)) {
                error = "无法删除未引用的数据块: " + path.string(); return false;
            }
        }
        if (ec) { error = "扫描数据块失败: " + ec.message(); return false; }
        it.increment(ec);
        if (ec) { error = "扫描数据块失败: " + ec.message(); return false; }
    }
    return true;
}

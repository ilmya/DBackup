#include "core/archive.h"
#include "core/archive_internal.h"
#include "core/linux_file.h"
#include "core/repository.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using dbackup::archive_format::Entry;
using namespace dbackup::linux_file;

TEST(LinuxRestore, CancellationAfterContentWritePreservesExistingFile) {
    test_support::TemporaryDirectory temporary("dbackup-cancel-restore");
    const fs::path destination = temporary.path() / "destination";
    fs::create_directories(destination);
    test_support::writeText(destination / "file", "original");
    Entry entry;
    entry.file.relativePath = "file";
    entry.file.type = 0;
    entry.file.metadata.mode = S_IFREG | 0600;
    entry.file.metadata.uid = getuid();
    entry.file.metadata.gid = getgid();
    std::atomic_bool cancelled{false};
    OperationContext context;
    context.cancelled = &cancelled;
    std::string error;
    const auto writer = [&](const Entry&, int fd, const OperationContext&, std::string&) {
        if (::write(fd, "replacement", 11) != 11)
            return false;
        cancelled = true;
        return true;
    };
    EXPECT_FALSE(dbackup::archive_restore::restore({entry}, destination, context, error, writer));
    EXPECT_EQ(test_support::readText(destination / "file"), "original");
    size_t count = 0;
    for (const auto& ignored : fs::directory_iterator(destination)) {
        (void)ignored;
        ++count;
    }
    EXPECT_EQ(count, 1u);
}

namespace {
Entry item(std::string name, int type, mode_t mode) {
    Entry entry;
    entry.file.relativePath = std::move(name);
    entry.file.type = type;
    entry.file.metadata.mode = mode;
    entry.file.metadata.uid = getuid();
    entry.file.metadata.gid = getgid();
    entry.file.metadata.mtimeNs = 1700000000123456789LL;
    return entry;
}
} // namespace

TEST(LinuxRestore, RestoresFilesAndLinks) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    Entry dir = item("dir", 1, 0750), file = item("dir/data", 0, 0640);
    file.data = {'a', 'b', 'c'};
    Entry hard = item("dir/hard", 3, 0640);
    hard.file.linkTarget = "dir/data";
    Entry link = item("dir/link", 2, 0777);
    link.file.linkTarget = "data";
    std::string error;
    ASSERT_TRUE(dbackup::archive_restore::restore({dir, file, hard, link},
                                                  fs::path(root) / "output", {}, error))
        << error;
    struct stat a {
    }, b{}, d{};
    ASSERT_EQ(stat((fs::path(root) / "output/dir/data").c_str(), &a), 0);
    ASSERT_EQ(stat((fs::path(root) / "output/dir/hard").c_str(), &b), 0);
    ASSERT_EQ(stat((fs::path(root) / "output/dir").c_str(), &d), 0);
    EXPECT_EQ(a.st_ino, b.st_ino);
    EXPECT_EQ(a.st_mode & 0777, 0640u);
    EXPECT_EQ(d.st_mode & 0777, 0750u);
    EXPECT_EQ(a.st_mtim.tv_nsec, 123456789);
    EXPECT_EQ(fs::read_symlink(fs::path(root) / "output/dir/link"), "data");
}

TEST(LinuxRestore, RejectsEscapesAndExistingSymlinkParents) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root), destination = base / "output", outside = base / "outside";
    fs::create_directories(destination);
    fs::create_directories(outside);
    fs::create_directory_symlink(outside, destination / "link");
    Entry file = item("link/escape", 0, 0600);
    file.data = {'x'};
    std::string error;
    EXPECT_FALSE(dbackup::archive_restore::restore({file}, destination, {}, error));
    EXPECT_FALSE(fs::exists(outside / "escape"));
    Entry traversal = item("../escape", 0, 0600);
    EXPECT_FALSE(dbackup::archive_restore::restore({traversal}, destination, {}, error));
    Entry link = item("bad", 2, 0777);
    link.file.linkTarget = "../../outside";
    EXPECT_FALSE(dbackup::archive_restore::restore({link}, destination, {}, error));
}

TEST(LinuxRestore, SkipsPermissionFailuresAndKeepsOtherContent) {
    if (geteuid() == 0)
        GTEST_SKIP() << "需要非 root 用户";
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    const fs::path base(root), output = base / "output";
    fs::create_directories(output / "blocked");
    ASSERT_EQ(chmod((output / "blocked").c_str(), 0500), 0);
    Entry blocked = item("blocked/file", 0, 0640);
    blocked.data = {'n', 'o'};
    Entry other = item("other", 0, 0640);
    other.data = {'o', 'k'};
    other.file.metadata.uid = 65534;
    std::string error;
    ASSERT_TRUE(dbackup::archive_restore::restore({blocked, other}, output, {}, error)) << error;
    EXPECT_NE(error.find("blocked"), std::string::npos);
    EXPECT_NE(error.find("other"), std::string::npos);
    EXPECT_FALSE(fs::exists(output / "blocked/file"));
    std::ifstream input(output / "other");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "ok");
    chmod((output / "blocked").c_str(), 0700);
}

TEST(LinuxFile, RelativePathsRejectTraversal) {
    EXPECT_TRUE(validRelativePath("dir/file"));
    EXPECT_FALSE(validRelativePath("/etc/passwd"));
    EXPECT_FALSE(validRelativePath("dir/../file"));
}

TEST(LinuxFile, DirectoryCreationRejectsSymlinkTraversal) {
    test_support::TemporaryDirectory temporary("dbackup-filesystem");
    const auto root = temporary.path();
    fs::path base(root);
    std::string error;
    ASSERT_TRUE(ensureDirectories(base / "real", error)) << error;
    fs::create_directory_symlink(base / "real", base / "link");
    EXPECT_FALSE(ensureDirectories(base / "link" / "nested", error));
    EXPECT_FALSE(fs::exists(base / "real" / "nested"));
}

TEST(LinuxFile, MetadataPreservesModeAndTime) {
    test_support::TemporaryDirectory temporary("dbackup-filesystem");
    const auto root = temporary.path();
    fs::path file = fs::path(root) / "item";
    std::ofstream(file) << "test";
    ASSERT_EQ(chmod(file.c_str(), 0640), 0);
    Metadata original, restored;
    std::string error;
    ASSERT_TRUE(readMetadata(file, original, error)) << error;
    const auto archive = fs::path(root) / "backup.abk";
    const auto destination = fs::path(root) / "restored";
    ASSERT_TRUE(ArchiveService::create(file.string(), archive.string(), error)) << error;
    ASSERT_TRUE(ArchiveService::extract(archive.string(), destination.string(), error)) << error;
    ASSERT_TRUE(readMetadata(destination / "item", restored, error)) << error;
    EXPECT_EQ(restored.mode & 07777, 0640u);
    EXPECT_EQ(restored.mtimeNs, original.mtimeNs);
    EXPECT_EQ(restored.uid, original.uid);
    EXPECT_EQ(restored.gid, original.gid);
}

TEST(SpecialFiles, SocketRoundTripsInBothArchivesAndSnapshot) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    const fs::path base(root), source = base / "source";
    fs::create_directory(source);
    std::ofstream(source / "file") << "content";
    const auto socketPath = (source / "socket").string();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(socketPath.size(), sizeof(address.sun_path));
    std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
    ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(fd, 1), 0);
    for (auto format : {PackFormat::Custom, PackFormat::Tar}) {
        PackOptions options;
        options.format = format;
        options.compression = Compression::None;
        const auto archive = base / (format == PackFormat::Tar ? "backup.tar" : "backup.abk");
        const auto output = base / (format == PackFormat::Tar ? "tar" : "custom");
        std::string error;
        ASSERT_TRUE(ArchiveService::create(source.string(), archive.string(), options, error))
            << error;
        ASSERT_TRUE(ArchiveService::extract(archive.string(), output.string(), error)) << error;
        struct stat info {};
        ASSERT_EQ(lstat((output / "socket").c_str(), &info), 0);
        EXPECT_TRUE(S_ISSOCK(info.st_mode));
        EXPECT_NE(error.find("套接字节点"), std::string::npos);
        const ino_t priorInode = info.st_ino;
        ASSERT_TRUE(ArchiveService::extract(archive.string(), output.string(), error)) << error;
        EXPECT_NE(error.find("socket"), std::string::npos);
        ASSERT_EQ(lstat((output / "socket").c_str(), &info), 0);
        EXPECT_EQ(info.st_ino, priorInode);
    }
    const auto occupied = base / "occupied";
    fs::create_directory(occupied);
    const auto occupiedPath = (occupied / "socket").string();
    int active = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(active, 0);
    sockaddr_un activeAddress{};
    activeAddress.sun_family = AF_UNIX;
    ASSERT_LT(occupiedPath.size(), sizeof(activeAddress.sun_path));
    std::copy(occupiedPath.begin(), occupiedPath.end(), activeAddress.sun_path);
    ASSERT_EQ(bind(active, reinterpret_cast<sockaddr*>(&activeAddress), sizeof(activeAddress)), 0);
    ASSERT_EQ(listen(active, 1), 0);
    struct stat before {
    }, after{};
    ASSERT_EQ(lstat(occupiedPath.c_str(), &before), 0);
    std::string conflictError;
    ASSERT_TRUE(
        ArchiveService::extract((base / "backup.abk").string(), occupied.string(), conflictError))
        << conflictError;
    EXPECT_NE(conflictError.find("socket"), std::string::npos);
    ASSERT_EQ(lstat(occupiedPath.c_str(), &after), 0);
    EXPECT_EQ(before.st_ino, after.st_ino);
    int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    EXPECT_EQ(connect(client, reinterpret_cast<sockaddr*>(&activeAddress), sizeof(activeAddress)),
              0);
    close(client);
    close(active);
    LocalRepository repo((base / "repo").string());
    SnapshotOptions options;
    options.sources = {source.string()};
    options.password = "test-only";
    SnapshotInfo snapshot;
    std::string error;
    ASSERT_TRUE(repo.createSnapshot(options, snapshot, error)) << error;
    ASSERT_TRUE(
        repo.restoreSnapshot(snapshot.id, (base / "snapshot").string(), options.password, error))
        << error;
    struct stat info {};
    ASSERT_EQ(lstat((base / "snapshot/source/socket").c_str(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    EXPECT_EQ(listen(fd, 1), 0);
    close(fd);
}

TEST(SpecialFiles, UnreadableSourceCreatesPartialSnapshotWithoutLosingReadableFiles) {
    if (geteuid() == 0)
        GTEST_SKIP() << "需要非 root 用户";
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    const fs::path base(root), source = base / "source";
    fs::create_directory(source);
    std::ofstream(source / "readable") << "keep";
    std::ofstream(source / "private") << "secret";
    LocalRepository repo((base / "repo").string());
    SnapshotOptions options;
    options.sources = {source.string()};
    options.password = "test-only";
    SnapshotInfo complete;
    std::string error;
    ASSERT_TRUE(repo.createSnapshot(options, complete, error)) << error;
    EXPECT_FALSE(complete.partial);
    ASSERT_EQ(chmod((source / "private").c_str(), 0), 0);
    PackOptions pack;
    ASSERT_TRUE(
        ArchiveService::create(source.string(), (base / "backup.abk").string(), pack, error))
        << error;
    EXPECT_NE(error.find("private"), std::string::npos);
    ASSERT_TRUE(
        ArchiveService::extract((base / "backup.abk").string(), (base / "output").string(), error))
        << error;
    EXPECT_TRUE(fs::exists(base / "output/readable"));
    EXPECT_FALSE(fs::exists(base / "output/private"));
    SnapshotInfo snapshot;
    ASSERT_TRUE(repo.createSnapshot(options, snapshot, error)) << error;
    EXPECT_TRUE(snapshot.partial);
    ASSERT_FALSE(snapshot.warnings.empty());
    std::vector<SnapshotInfo> listed;
    ASSERT_TRUE(repo.listSnapshots(listed, error)) << error;
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_TRUE(listed[0].partial);
    EXPECT_FALSE(listed[0].warnings.empty());
    EXPECT_EQ(listed[1].id, complete.id);
    EXPECT_FALSE(listed[1].partial);
    ASSERT_TRUE(
        repo.restoreSnapshot(snapshot.id, (base / "restored").string(), options.password, error))
        << error;
    EXPECT_TRUE(fs::exists(base / "restored/source/readable"));
    EXPECT_FALSE(fs::exists(base / "restored/source/private"));
    chmod((source / "private").c_str(), 0600);
}

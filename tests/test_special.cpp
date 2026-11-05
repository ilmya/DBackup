#include "core/archive.h"
#include "core/repository.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(SpecialFiles, SocketRoundTripsInBothArchivesAndSnapshot) {
    std::string name = (fs::temp_directory_path() / "dbackup-special-XXXXXX").string();
    const char *root = mkdtemp(name.data()); ASSERT_NE(root, nullptr);
    const fs::path base(root), source = base / "source";
    fs::create_directory(source);
    std::ofstream(source / "file") << "content";
    const auto socketPath = (source / "socket").string();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); ASSERT_GE(fd, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    ASSERT_LT(socketPath.size(), sizeof(address.sun_path));
    std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
    ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(fd, 1), 0);
    for (auto format : {PackFormat::Custom, PackFormat::Tar}) {
        PackOptions options; options.format = format; options.compression = Compression::None;
        const auto archive = base / (format == PackFormat::Tar ? "backup.tar" : "backup.abk");
        const auto output = base / (format == PackFormat::Tar ? "tar" : "custom");
        std::string error;
        ASSERT_TRUE(ArchiveService::create(source.string(), archive.string(), options, error)) << error;
        ASSERT_TRUE(ArchiveService::extract(archive.string(), output.string(), error)) << error;
        struct stat info{}; ASSERT_EQ(lstat((output / "socket").c_str(), &info), 0); EXPECT_TRUE(S_ISSOCK(info.st_mode));
        EXPECT_NE(error.find("套接字节点"), std::string::npos);
        const ino_t priorInode = info.st_ino;
        ASSERT_TRUE(ArchiveService::extract(archive.string(), output.string(), error)) << error;
        EXPECT_NE(error.find("socket"), std::string::npos);
        ASSERT_EQ(lstat((output / "socket").c_str(), &info), 0); EXPECT_EQ(info.st_ino, priorInode);
    }
    const auto occupied = base / "occupied";
    fs::create_directory(occupied);
    const auto occupiedPath = (occupied / "socket").string();
    int active = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); ASSERT_GE(active, 0);
    sockaddr_un activeAddress{}; activeAddress.sun_family = AF_UNIX;
    ASSERT_LT(occupiedPath.size(), sizeof(activeAddress.sun_path));
    std::copy(occupiedPath.begin(), occupiedPath.end(), activeAddress.sun_path);
    ASSERT_EQ(bind(active, reinterpret_cast<sockaddr *>(&activeAddress), sizeof(activeAddress)), 0);
    ASSERT_EQ(listen(active, 1), 0);
    struct stat before{}, after{}; ASSERT_EQ(lstat(occupiedPath.c_str(), &before), 0);
    std::string conflictError;
    ASSERT_TRUE(ArchiveService::extract((base / "backup.abk").string(), occupied.string(), conflictError)) << conflictError;
    EXPECT_NE(conflictError.find("socket"), std::string::npos);
    ASSERT_EQ(lstat(occupiedPath.c_str(), &after), 0); EXPECT_EQ(before.st_ino, after.st_ino);
    int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); ASSERT_GE(client, 0);
    EXPECT_EQ(connect(client, reinterpret_cast<sockaddr *>(&activeAddress), sizeof(activeAddress)), 0);
    close(client); close(active);
    LocalRepository repo((base / "repo").string());
    SnapshotOptions options; options.sources = {source.string()}; options.password = "test-only";
    SnapshotInfo snapshot; std::string error;
    ASSERT_TRUE(repo.createSnapshot(options, snapshot, error)) << error;
    ASSERT_TRUE(repo.restoreSnapshot(snapshot.id, (base / "snapshot").string(), options.password, error)) << error;
    struct stat info{}; ASSERT_EQ(lstat((base / "snapshot/source/socket").c_str(), &info), 0); EXPECT_TRUE(S_ISSOCK(info.st_mode));
    EXPECT_EQ(listen(fd, 1), 0);
    close(fd); fs::remove_all(base);
}

TEST(SpecialFiles, UnreadableSourceCreatesPartialSnapshotWithoutLosingReadableFiles) {
    if (geteuid() == 0) GTEST_SKIP() << "需要非 root 用户";
    std::string name = (fs::temp_directory_path() / "dbackup-partial-XXXXXX").string();
    const char *root = mkdtemp(name.data()); ASSERT_NE(root, nullptr);
    const fs::path base(root), source = base / "source"; fs::create_directory(source);
    std::ofstream(source / "readable") << "keep";
    std::ofstream(source / "private") << "secret";
    LocalRepository repo((base / "repo").string()); SnapshotOptions options;
    options.sources = {source.string()}; options.password = "test-only";
    SnapshotInfo complete; std::string error;
    ASSERT_TRUE(repo.createSnapshot(options, complete, error)) << error;
    EXPECT_FALSE(complete.partial);
    ASSERT_EQ(chmod((source / "private").c_str(), 0), 0);
    PackOptions pack;
    ASSERT_TRUE(ArchiveService::create(source.string(), (base / "backup.abk").string(), pack, error)) << error;
    EXPECT_NE(error.find("private"), std::string::npos);
    ASSERT_TRUE(ArchiveService::extract((base / "backup.abk").string(), (base / "output").string(), error)) << error;
    EXPECT_TRUE(fs::exists(base / "output/readable")); EXPECT_FALSE(fs::exists(base / "output/private"));
    SnapshotInfo snapshot; ASSERT_TRUE(repo.createSnapshot(options, snapshot, error)) << error;
    EXPECT_TRUE(snapshot.partial); ASSERT_FALSE(snapshot.warnings.empty());
    std::vector<SnapshotInfo> listed; ASSERT_TRUE(repo.listSnapshots(listed, error)) << error;
    ASSERT_EQ(listed.size(), 2u); EXPECT_TRUE(listed[0].partial); EXPECT_FALSE(listed[0].warnings.empty());
    EXPECT_EQ(listed[1].id, complete.id); EXPECT_FALSE(listed[1].partial);
    ASSERT_TRUE(repo.restoreSnapshot(snapshot.id, (base / "restored").string(), options.password, error)) << error;
    EXPECT_TRUE(fs::exists(base / "restored/source/readable")); EXPECT_FALSE(fs::exists(base / "restored/source/private"));
    chmod((source / "private").c_str(), 0600); fs::remove_all(base);
}

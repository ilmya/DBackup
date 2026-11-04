#include "core/repository.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(LinuxRepository, SnapshotsDeduplicateRestoreAndPrune) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a.txt") << "hello";
    LocalRepository repository((base / "repository").string());
    SnapshotOptions options; options.sources = {(base / "source").string()}; options.password = "secret"; options.chunkSize = 4;
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    EXPECT_GT(first.storedBytes, 0u);
    EXPECT_EQ(second.storedBytes, 0u);
    ASSERT_TRUE(repository.prune(1, error)) << error;
    std::vector<SnapshotInfo> snapshots;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].id, second.id);
    ASSERT_TRUE(repository.restoreSnapshot(second.id, (base / "restored").string(), "secret", error)) << error;
    std::ifstream input(base / "restored/source/a.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "hello");
    EXPECT_FALSE(repository.initialize("wrong", error));
    fs::remove_all(base);
}

TEST(LinuxRepository, DeletingSnapshotPreservesOtherSnapshot) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a.txt") << "shared content";
    LocalRepository repository((base / "repository").string());
    SnapshotOptions options; options.sources = {(base / "source").string()}; options.password = "secret";
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    ASSERT_TRUE(repository.deleteSnapshot(first.id, error)) << error;
    std::vector<SnapshotInfo> snapshots;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].id, second.id);
    ASSERT_TRUE(repository.restoreSnapshot(second.id, (base / "restored").string(), "secret", error)) << error;
    std::ifstream input(base / "restored/source/a.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "shared content");
    fs::remove_all(base);
}

TEST(LinuxRepository, RejectsOldConfigAndPreservesExistingData) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    std::ofstream(base / "config.bin", std::ios::binary) << "DBR1old-data";
    LocalRepository repository(base.string());
    std::string error;
    EXPECT_FALSE(repository.initialize("secret", error));
    std::ifstream file(base / "config.bin", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "DBR1old-data");
    fs::remove_all(base);
}

TEST(LinuxRepository, CorruptedRetainedManifestPreventsPruning) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a") << "test";
    LocalRepository repository((base / "repo").string());
    SnapshotOptions options; options.sources = {(base / "source").string()}; options.password = "secret";
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    std::ofstream(base / "repo/snapshots" / (second.id + ".manifest"), std::ios::trunc) << "corrupt";
    EXPECT_FALSE(repository.prune(1, error));
    EXPECT_TRUE(fs::exists(base / "repo/snapshots" / (first.id + ".manifest")));
    fs::remove_all(base);
}

TEST(LinuxRepository, PruneWaitsForConcurrentRepositoryOperation) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root), config = base / "repo/config.bin";
    LocalRepository repository((base / "repo").string());
    std::string error;
    ASSERT_TRUE(repository.initialize("secret", error)) << error;
    int fd = open(config.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(flock(fd, LOCK_SH), 0);
    auto result = std::async(std::launch::async, [&] { return repository.prune(0, error); });
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    ASSERT_EQ(flock(fd, LOCK_UN), 0);
    close(fd);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(result.get()) << error;
    fs::remove_all(base);
}

TEST(LinuxRepository, RefusesSymlinkedChunkDirectory) {
    char name[] = "/tmp/dbackup-repository-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root), repositoryRoot = base / "repo";
    fs::create_directory(base / "source");
    fs::create_directory(base / "outside");
    std::ofstream(base / "source/file") << "test";
    LocalRepository repository(repositoryRoot.string());
    std::string error;
    ASSERT_TRUE(repository.initialize("secret", error)) << error;
    ASSERT_TRUE(fs::remove(repositoryRoot / "chunks"));
    fs::create_directory_symlink(base / "outside", repositoryRoot / "chunks");
    SnapshotOptions options; options.sources = {(base / "source").string()}; options.password = "secret";
    SnapshotInfo snapshot;
    EXPECT_FALSE(repository.createSnapshot(options, snapshot, error));
    EXPECT_TRUE(fs::is_empty(base / "outside"));
    fs::remove_all(base);
}

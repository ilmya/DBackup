#include "core/archive_internal.h"
#include "core/repository.h"
#include "test_support.h"
#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(LinuxRepository, SnapshotsDeduplicateRestoreAndPrune) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a.txt") << "hello";
    LocalRepository repository((base / "repository").string());
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "secret";
    options.chunkSize = 4;
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    EXPECT_GT(first.storedBytes, 0u);
    EXPECT_EQ(second.storedBytes, 0u);
    ASSERT_TRUE(repository.prune(10, error)) << error;
    std::vector<SnapshotInfo> snapshots;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    ASSERT_EQ(snapshots.size(), 2u);
    ASSERT_TRUE(repository.prune(1, error)) << error;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].id, second.id);
    ASSERT_TRUE(
        repository.restoreSnapshot(second.id, (base / "restored").string(), "secret", error))
        << error;
    std::ifstream input(base / "restored/source/a.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "hello");
    EXPECT_FALSE(repository.initialize("wrong", error));
    ASSERT_TRUE(repository.prune(0, error)) << error;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    EXPECT_TRUE(snapshots.empty());
}

TEST(LinuxRepository, CorruptedChunkDoesNotPublishAnyRestoredFiles) {
    test_support::TemporaryDirectory temporary("dbackup-corrupt-restore");
    const fs::path base = temporary.path();
    fs::create_directories(base / "source");
    test_support::writeText(base / "source/a.txt", "first file contents");
    test_support::writeText(base / "source/b.txt", "second file contents");
    LocalRepository repository((base / "repo").string());
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "test-only";
    options.chunkSize = 4;
    SnapshotInfo snapshot;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    fs::path chunk;
    for (const auto& item : fs::recursive_directory_iterator(base / "repo/chunks"))
        if (item.path().extension() == ".blk") {
            chunk = item.path();
            break;
        }
    ASSERT_FALSE(chunk.empty());
    test_support::writeText(chunk, "corrupted");
    fs::create_directories(base / "restored/source");
    test_support::writeText(base / "restored/source/a.txt", "original");
    EXPECT_FALSE(repository.restoreSnapshot(snapshot.id, (base / "restored").string(),
                                            options.password, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(test_support::readText(base / "restored/source/a.txt"), "original");
    EXPECT_FALSE(fs::exists(base / "restored/source/b.txt"));
}

TEST(LinuxRepository, OversizedStoredChunkIsRejectedBeforeRestore) {
    test_support::TemporaryDirectory temporary("dbackup-oversized-chunk");
    const fs::path base = temporary.path();
    fs::create_directories(base / "source");
    test_support::writeText(base / "source/a", "content");
    LocalRepository repository((base / "repo").string());
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "test-only";
    SnapshotInfo snapshot;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    fs::path chunk;
    for (const auto& item : fs::recursive_directory_iterator(base / "repo/chunks"))
        if (item.path().extension() == ".blk") {
            chunk = item.path();
            break;
        }
    ASSERT_FALSE(chunk.empty());
    ASSERT_EQ(::truncate(chunk.c_str(), 64LL * 1024 * 1024 + 37), 0);
    EXPECT_FALSE(repository.restoreSnapshot(snapshot.id, (base / "restored").string(),
                                            options.password, error));
    EXPECT_NE(error.find("大小超出限制"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(base / "restored/source/a"));
}

TEST(LinuxRepository, DeletingSnapshotPreservesOtherSnapshot) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a.txt") << "shared content";
    LocalRepository repository((base / "repository").string());
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "secret";
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    ASSERT_TRUE(repository.deleteSnapshot(first.id, error)) << error;
    std::vector<SnapshotInfo> snapshots;
    ASSERT_TRUE(repository.listSnapshots(snapshots, error)) << error;
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].id, second.id);
    ASSERT_TRUE(
        repository.restoreSnapshot(second.id, (base / "restored").string(), "secret", error))
        << error;
    std::ifstream input(base / "restored/source/a.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "shared content");
}

TEST(LinuxRepository, PreviewAndBackupModesApplyTheSamePathRules) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    const fs::path base(root), source = base / "source";
    fs::create_directories(source / "docs/private");
    std::ofstream(source / "docs/report.txt") << "selected";
    std::ofstream(source / "docs/private/secret.txt") << "excluded";
    FilterOptions filter;
    filter.pathRules = {{true, "docs/*"}, {false, "docs/private/*"}};
    BackupPreview preview;
    std::string error;
    ASSERT_TRUE(ArchiveService::preview({source.string()}, filter, preview, error)) << error;
    EXPECT_EQ(preview.includedFiles, 1u);
    PackOptions pack;
    pack.filter = filter;
    const auto archive = (base / "backup.abk").string();
    ASSERT_TRUE(ArchiveService::create(source.string(), archive, pack, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(archive, (base / "archive-output").string(), error))
        << error;
    EXPECT_TRUE(fs::exists(base / "archive-output/docs/report.txt"));
    EXPECT_FALSE(fs::exists(base / "archive-output/docs/private/secret.txt"));
    LocalRepository repository((base / "repo").string());
    SnapshotOptions options;
    options.sources = {source.string()};
    options.password = "test-only";
    options.filter = filter;
    SnapshotInfo snapshot;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    BackupPreview snapshotPreview;
    ASSERT_TRUE(ArchiveService::preview({source.string()}, filter, snapshotPreview, error, true))
        << error;
    std::ifstream manifestFile(base / "repo/snapshots" / (snapshot.id + ".manifest"),
                               std::ios::binary);
    std::vector<uint8_t> manifestBytes{std::istreambuf_iterator<char>(manifestFile), {}};
    std::vector<dbackup::archive_format::Entry> manifestEntries;
    ASSERT_TRUE(dbackup::archive_format::decode(manifestBytes, "test-only", manifestEntries, error))
        << error;
    EXPECT_EQ(snapshotPreview.includedFiles + snapshotPreview.includedDirectories,
              manifestEntries.size());
    EXPECT_EQ(snapshotPreview.includedFiles, preview.includedFiles);
    ASSERT_TRUE(repository.restoreSnapshot(snapshot.id, (base / "snapshot-output").string(),
                                           "test-only", error))
        << error;
    EXPECT_TRUE(fs::exists(base / "snapshot-output/source/docs/report.txt"));
    EXPECT_FALSE(fs::exists(base / "snapshot-output/source/docs/private/secret.txt"));
}

TEST(LinuxRepository, RejectsOldConfigAndPreservesExistingData) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root);
    std::ofstream(base / "config.bin", std::ios::binary) << "DBR1old-data";
    LocalRepository repository(base.string());
    std::string error;
    EXPECT_FALSE(repository.initialize("secret", error));
    std::ifstream file(base / "config.bin", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "DBR1old-data");
}

TEST(LinuxRepository, CorruptedRetainedManifestPreventsPruning) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root);
    fs::create_directory(base / "source");
    std::ofstream(base / "source/a") << "test";
    LocalRepository repository((base / "repo").string());
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "secret";
    SnapshotInfo first, second;
    std::string error;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    ASSERT_TRUE(repository.createSnapshot(options, second, error)) << error;
    std::ofstream(base / "repo/snapshots" / (second.id + ".manifest"), std::ios::trunc)
        << "corrupt";
    EXPECT_FALSE(repository.prune(1, error));
    EXPECT_TRUE(fs::exists(base / "repo/snapshots" / (first.id + ".manifest")));
}

TEST(LinuxRepository, PruneWaitsForConcurrentRepositoryOperation) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
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
}

TEST(LinuxRepository, RefusesSymlinkedChunkDirectory) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root), repositoryRoot = base / "repo";
    fs::create_directory(base / "source");
    fs::create_directory(base / "outside");
    std::ofstream(base / "source/file") << "test";
    LocalRepository repository(repositoryRoot.string());
    std::string error;
    ASSERT_TRUE(repository.initialize("secret", error)) << error;
    ASSERT_TRUE(fs::remove(repositoryRoot / "chunks"));
    fs::create_directory_symlink(base / "outside", repositoryRoot / "chunks");
    SnapshotOptions options;
    options.sources = {(base / "source").string()};
    options.password = "secret";
    SnapshotInfo snapshot;
    EXPECT_FALSE(repository.createSnapshot(options, snapshot, error));
    EXPECT_TRUE(fs::is_empty(base / "outside"));
}

TEST(LocalRepositoryTest, SingleFileSourceDoesNotIncludeItsSiblingsAndHonorsCancellation) {
    test_support::TemporaryDirectory temporary("dbackup-single-source");
    const auto base = temporary.path();
    const std::string root = (base / "repo").string(), source = (base / "source").string(),
                      restore = (base / "restore").string();
    std::filesystem::create_directory(source);
    test_support::writeText(source + "/selected.txt", "selected");
    test_support::writeText(source + "/sibling.txt", "must not be copied");
    LocalRepository repository(root);
    std::string error;
    SnapshotOptions options;
    options.sources = {source + "/selected.txt"};
    options.password = "secret";
    SnapshotInfo info;
    ASSERT_TRUE(repository.createSnapshot(options, info, error)) << error;
    EXPECT_EQ(info.fileCount, 1u);
    ASSERT_TRUE(repository.restoreSnapshot(info.id, restore, "secret", error)) << error;
    EXPECT_EQ(test_support::readText(restore + "/selected.txt"), "selected");
    EXPECT_FALSE(std::filesystem::exists(restore + "/sibling.txt"));
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(repository.createSnapshot(options, info, error, context));
}

TEST(LocalRepositoryTest, RejectsUnsafeSnapshotIdsBeforeFilesystemAccess) {
    test_support::TemporaryDirectory temporary("dbackup-unsafe-snapshot-id");
    const auto base = temporary.path();
    const std::string root = (base / "repo").string(), destination = (base / "restore").string();
    LocalRepository repository(root);
    std::string error;
    EXPECT_FALSE(repository.restoreSnapshot("../../outside", destination, "secret", error));
    EXPECT_FALSE(repository.deleteSnapshot("../../outside", error));
    EXPECT_FALSE(std::filesystem::exists((base / "outside.manifest").string()));
}

TEST(LocalRepositoryTest, DoesNotOverwriteCorruptConfiguration) {
    test_support::TemporaryDirectory temporary("dbackup-corrupt-config");
    const std::string root = (temporary.path() / "repo").string();
    LocalRepository repository(root);
    std::string error;
    ASSERT_TRUE(repository.initialize("secret", error)) << error;
    const std::string config = root + "/config.bin";
    {
        std::ofstream file(config, std::ios::binary | std::ios::trunc);
        file << "broken config";
    }
    EXPECT_FALSE(repository.initialize("new password", error));
    std::ifstream file(config, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "broken config");
}

TEST(LocalRepositoryTest, RejectsWrongPasswordForExistingSnapshots) {
    test_support::TemporaryDirectory temporary("dbackup-wrong-password");
    const auto base = temporary.path();
    const std::string root = (base / "repo").string(), source = (base / "source").string(),
                      restore = (base / "restore").string();
    std::filesystem::create_directory(source);
    test_support::writeText(source + "/file.txt", "original");
    SnapshotOptions options;
    options.sources = {source};
    options.password = "secret";
    LocalRepository repository(root);
    std::string error;
    SnapshotInfo first;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    options.password = "wrong password";
    LocalRepository wrongPassword(root);
    SnapshotInfo second;
    EXPECT_FALSE(wrongPassword.createSnapshot(options, second, error));
    ASSERT_TRUE(repository.restoreSnapshot(first.id, restore, "secret", error)) << error;
    EXPECT_EQ(test_support::readText(restore + "/" + source.substr(source.find_last_of("//") + 1) +
                                     "/file.txt"),
              "original");
}

TEST(LocalRepositoryTest, PreservesEmptySelectedDirectory) {
    test_support::TemporaryDirectory temporary("dbackup-empty-source");
    const auto base = temporary.path();
    const std::string root = (base / "repo").string(), source = (base / "source").string(),
                      restore = (base / "restore").string();
    std::filesystem::create_directory(source);
    SnapshotOptions options;
    options.sources = {source};
    options.password = "secret";
    LocalRepository repository(root);
    std::string error;
    SnapshotInfo snapshot;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    ASSERT_TRUE(repository.restoreSnapshot(snapshot.id, restore, "secret", error)) << error;
    const std::string restoredRoot = restore + "/" + source.substr(source.find_last_of("//") + 1);
    EXPECT_TRUE(std::filesystem::is_directory(restoredRoot));
}

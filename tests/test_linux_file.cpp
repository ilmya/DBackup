#include "core/linux_file.h"
#include "core/archive.h"
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace dbackup::linux_file;

TEST(LinuxFile, RelativePathsRejectTraversal) {
    EXPECT_TRUE(validRelativePath("dir/file"));
    EXPECT_FALSE(validRelativePath("/etc/passwd"));
    EXPECT_FALSE(validRelativePath("dir/../file"));
}

TEST(LinuxFile, DirectoryCreationRejectsSymlinkTraversal) {
    std::string name = (std::filesystem::temp_directory_path() / "dbackup-file-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    std::string error;
    ASSERT_TRUE(ensureDirectories(base / "real", error)) << error;
    fs::create_directory_symlink(base / "real", base / "link");
    EXPECT_FALSE(ensureDirectories(base / "link" / "nested", error));
    EXPECT_FALSE(fs::exists(base / "real" / "nested"));
    fs::remove_all(base);
}

TEST(LinuxFile, MetadataPreservesModeAndTime) {
    std::string name = (std::filesystem::temp_directory_path() / "dbackup-file-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
    ASSERT_NE(root, nullptr);
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
    fs::remove_all(root);
}

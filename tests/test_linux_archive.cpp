#include "archive/archive.h"
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST(LinuxArchive, EncryptedRoundTripWithLinksAndFilter) {
    char name[] = "/tmp/dbackup-archive-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root), source = base / "source";
    fs::create_directories(source / "empty");
    std::ofstream(source / "文件.txt") << "backup content";
    ASSERT_EQ(chmod((source / "文件.txt").c_str(), 0640), 0);
    fs::create_hard_link(source / "文件.txt", source / "hard.txt");
    fs::create_symlink("文件.txt", source / "link");
    PackOptions options; options.password = "correct";
    std::string error;
    ASSERT_TRUE(ArchiveService::create(source.string(), (base / "backup.abk").string(), options, error)) << error;
    EXPECT_FALSE(ArchiveService::extract((base / "backup.abk").string(), (base / "wrong").string(), error, "wrong"));
    ASSERT_TRUE(ArchiveService::extract((base / "backup.abk").string(), (base / "restored").string(), error, "correct")) << error;
    EXPECT_TRUE(fs::is_directory(base / "restored/empty"));
    EXPECT_EQ(fs::read_symlink(base / "restored/link"), "文件.txt");
    struct stat first{}, second{};
    ASSERT_EQ(stat((base / "restored/文件.txt").c_str(), &first), 0);
    ASSERT_EQ(stat((base / "restored/hard.txt").c_str(), &second), 0);
    EXPECT_EQ(first.st_ino, second.st_ino);
    EXPECT_EQ(first.st_mode & 0777, 0640u);
    std::ifstream content(base / "restored/文件.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(content), {}), "backup content");
    fs::remove_all(base);
}

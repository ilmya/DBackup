#include "core/archive_internal.h"
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using dbackup::archive_format::Entry;

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
}

TEST(LinuxRestore, RestoresFilesAndLinks) {
    std::string name = (std::filesystem::temp_directory_path() / "dbackup-restore-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
    ASSERT_NE(root, nullptr);
    Entry dir = item("dir", 1, 0750), file = item("dir/data", 0, 0640);
    file.data = {'a', 'b', 'c'};
    Entry hard = item("dir/hard", 3, 0640); hard.file.linkTarget = "dir/data";
    Entry link = item("dir/link", 2, 0777); link.file.linkTarget = "data";
    std::string error;
    ASSERT_TRUE(dbackup::archive_restore::restore({dir, file, hard, link}, fs::path(root) / "output", {}, error)) << error;
    struct stat a{}, b{}, d{};
    ASSERT_EQ(stat((fs::path(root) / "output/dir/data").c_str(), &a), 0);
    ASSERT_EQ(stat((fs::path(root) / "output/dir/hard").c_str(), &b), 0);
    ASSERT_EQ(stat((fs::path(root) / "output/dir").c_str(), &d), 0);
    EXPECT_EQ(a.st_ino, b.st_ino);
    EXPECT_EQ(a.st_mode & 0777, 0640u);
    EXPECT_EQ(d.st_mode & 0777, 0750u);
    EXPECT_EQ(a.st_mtim.tv_nsec, 123456789);
    EXPECT_EQ(fs::read_symlink(fs::path(root) / "output/dir/link"), "data");
    fs::remove_all(root);
}

TEST(LinuxRestore, RejectsEscapesAndExistingSymlinkParents) {
    std::string name = (std::filesystem::temp_directory_path() / "dbackup-restore-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
    ASSERT_NE(root, nullptr);
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
    Entry link = item("bad", 2, 0777); link.file.linkTarget = "../../outside";
    EXPECT_FALSE(dbackup::archive_restore::restore({link}, destination, {}, error));
    fs::remove_all(base);
}

TEST(LinuxRestore, SkipsPermissionFailuresAndKeepsOtherContent) {
    if (geteuid() == 0) GTEST_SKIP() << "需要非 root 用户";
    std::string name = (fs::temp_directory_path() / "dbackup-permission-XXXXXX").string();
    const char *root = mkdtemp(name.data()); ASSERT_NE(root, nullptr);
    const fs::path base(root), output = base / "output";
    fs::create_directories(output / "blocked");
    ASSERT_EQ(chmod((output / "blocked").c_str(), 0500), 0);
    Entry blocked = item("blocked/file", 0, 0640); blocked.data = {'n', 'o'};
    Entry other = item("other", 0, 0640); other.data = {'o', 'k'};
    other.file.metadata.uid = 65534;
    std::string error;
    ASSERT_TRUE(dbackup::archive_restore::restore({blocked, other}, output, {}, error)) << error;
    EXPECT_NE(error.find("blocked"), std::string::npos);
    EXPECT_NE(error.find("other"), std::string::npos);
    EXPECT_FALSE(fs::exists(output / "blocked/file"));
    std::ifstream input(output / "other");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "ok");
    chmod((output / "blocked").c_str(), 0700);
    fs::remove_all(base);
}

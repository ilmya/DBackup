#include "core/archive_internal.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(LinuxScan, PreservesLinksAndAppliesFileFilter) {
    char name[] = "/tmp/dbackup-scan-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path base(root);
    fs::create_directory(base / "dir");
    std::ofstream(base / "dir" / "a.txt") << "data";
    fs::create_hard_link(base / "dir" / "a.txt", base / "dir" / "b.txt");
    fs::create_symlink("a.txt", base / "dir" / "link");
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    ASSERT_TRUE(dbackup::archive_scan::collect({(base / "dir").string()}, {}, {}, entries,
                                               nullptr, {}, error)) << error;
    unsigned regular = 0, hard = 0, symbolic = 0;
    for (const auto &entry : entries) {
        regular += entry.type == 0;
        hard += entry.type == 3;
        symbolic += entry.type == 2;
    }
    EXPECT_EQ(regular, 1u);
    EXPECT_EQ(hard, 1u);
    EXPECT_EQ(symbolic, 1u);
    FilterOptions filter; filter.extension = "txt";
    BackupPreview preview;
    ASSERT_TRUE(dbackup::archive_scan::collect({(base / "dir").string()}, {}, filter, entries,
                                               &preview, {}, error)) << error;
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(preview.excludedFiles, 1u);
    fs::remove_all(base);
}

TEST(LinuxScan, RefusesSpecialFilesAndHonorsCancellation) {
    char name[] = "/tmp/dbackup-scan-test-XXXXXX";
    const char *root = mkdtemp(name);
    ASSERT_NE(root, nullptr);
    fs::path fifo = fs::path(root) / "pipe";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    EXPECT_FALSE(dbackup::archive_scan::collect({root}, {}, {}, entries, nullptr, {}, error));
    std::atomic_bool cancelled{true};
    OperationContext context; context.cancelled = &cancelled;
    EXPECT_FALSE(dbackup::archive_scan::collect({fifo.string()}, {}, {}, entries, nullptr, context, error));
    fs::remove_all(root);
}

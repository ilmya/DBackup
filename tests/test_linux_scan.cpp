#include "core/archive_internal.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(LinuxScan, PreservesLinksAndAppliesFileFilter) {
    std::string name = (std::filesystem::temp_directory_path() / "dbackup-scan-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
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

TEST(LinuxScan, HonorsCancellation) {
    std::string name = (fs::temp_directory_path() / "dbackup-scan-test-XXXXXX").string();
    const char *root = mkdtemp(name.data());
    ASSERT_NE(root, nullptr);
    std::ofstream(fs::path(root) / "item") << "data";
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    std::atomic_bool cancelled{true};
    OperationContext context; context.cancelled = &cancelled;
    EXPECT_FALSE(dbackup::archive_scan::collect({root}, {}, {}, entries, nullptr, context, error));
    fs::remove_all(root);
}

TEST(LinuxScan, NestedSelectionExcludesAndReincludesWithNewFiles) {
    std::string name = (fs::temp_directory_path() / "dbackup-selection-XXXXXX").string();
    const char *root = mkdtemp(name.data()); ASSERT_NE(root, nullptr);
    const fs::path base(root), source = base / "source";
    fs::create_directories(source / "excluded/selected");
    std::ofstream(source / "new.txt") << "new";
    std::ofstream(source / "excluded/hidden.txt") << "hidden";
    std::ofstream(source / "excluded/selected/keep.txt") << "keep";
    FilterOptions filter;
    filter.sourceRules = {{source.string(), true}, {(source / "excluded").string(), false},
                          {(source / "excluded/selected").string(), true}};
    std::vector<dbackup::archive_scan::Entry> entries;
    BackupPreview preview; std::string error;
    ASSERT_TRUE(dbackup::archive_scan::collect({source.string()}, {}, filter, entries, &preview, {}, error)) << error;
    std::set<std::string> paths; for (const auto &entry : entries) paths.insert(entry.relativePath);
    EXPECT_TRUE(paths.count("new.txt"));
    EXPECT_FALSE(paths.count("excluded/hidden.txt"));
    EXPECT_TRUE(paths.count("excluded/selected/keep.txt"));
    EXPECT_EQ(preview.includedFiles, 2u);
    fs::remove_all(base);
}

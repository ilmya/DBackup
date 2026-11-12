// 验证Linux 文件扫描与筛选
#include "core/archive/archive_internal.h"
#include "core/selection/backup_policy.h"
#include "test_support.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST(LinuxScan, PreservesLinksAndAppliesFileFilter) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path base(root);
    fs::create_directory(base / "dir");
    std::ofstream(base / "dir" / "a.txt") << "data";
    fs::create_hard_link(base / "dir" / "a.txt", base / "dir" / "b.txt");
    fs::create_symlink("a.txt", base / "dir" / "link");
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    ASSERT_TRUE(dbackup::archive_scan::collect({(base / "dir").string()}, {}, {}, entries, nullptr,
                                               {}, error))
        << error;
    unsigned regular = 0, hard = 0, symbolic = 0;
    for (const auto& entry : entries) {
        regular += entry.type == 0;
        hard += entry.type == 3;
        symbolic += entry.type == 2;
    }
    EXPECT_EQ(regular, 1u);
    EXPECT_EQ(hard, 1u);
    EXPECT_EQ(symbolic, 1u);
    FilterOptions filter;
    filter.extension = "txt";
    BackupPreview preview;
    ASSERT_TRUE(dbackup::archive_scan::collect({(base / "dir").string()}, {}, filter, entries,
                                               &preview, {}, error))
        << error;
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(preview.excludedFiles, 1u);
}

TEST(LinuxScan, HonorsCancellation) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    std::ofstream(fs::path(root) / "item") << "data";
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(dbackup::archive_scan::collect({root}, {}, {}, entries, nullptr, context, error));
}

TEST(LinuxScan, PreviewCountsSpecialTypesAndFiltersFifo) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path source = fs::path(root) / "source";
    fs::create_directory(source);
    std::ofstream(source / "file") << "data";
    fs::create_symlink("file", source / "link");
    ASSERT_EQ(mkfifo((source / "pipe").c_str(), 0600), 0);
    FilterOptions filter;
    filter.typeMask = 1u << 4;
    BackupPreview preview;
    std::string error;
    ASSERT_TRUE(ArchiveService::preview({source.string()}, filter, preview, error, true)) << error;
    EXPECT_EQ(preview.includedTypes[4], 1u);
    EXPECT_EQ(preview.excludedTypes[0], 1u);
    EXPECT_EQ(preview.excludedTypes[2], 1u);
    OperationContext context;
    context.includeSourceDirectory = true;
    PackOptions options;
    options.filter = filter;
    const auto archive = (fs::path(root) / "backup.abk").string();
    ASSERT_TRUE(ArchiveService::create({source.string()}, archive, options, error, context))
        << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(archive, entries, error)) << error;
    std::set<std::string> paths;
    for (const auto& entry : entries)
        paths.insert(entry.relativePath);
    EXPECT_TRUE(paths.count("source/pipe"));
    EXPECT_FALSE(paths.count("source/file"));
    ASSERT_TRUE(ArchiveService::extract(archive, (fs::path(root) / "restore").string(), error))
        << error;
    struct stat pipeInfo {};
    ASSERT_EQ(lstat((fs::path(root) / "restore/source/pipe").c_str(), &pipeInfo), 0);
    EXPECT_TRUE(S_ISFIFO(pipeInfo.st_mode));
}

TEST(LinuxScan, PreviewReportsUnreadableFiles) {
    if (geteuid() == 0)
        GTEST_SKIP() << "requires an unprivileged user";
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    fs::path file = fs::path(root) / "private";
    std::ofstream(file) << "secret";
    ASSERT_EQ(chmod(file.c_str(), 0), 0);
    BackupPreview preview;
    std::string error;
    EXPECT_TRUE(ArchiveService::preview({root}, {}, preview, error, true));
    EXPECT_EQ(preview.unreadableFiles, 1u);
    EXPECT_NE(error.find("private"), std::string::npos);
    chmod(file.c_str(), 0600);
}

TEST(LinuxScan, NestedSelectionExcludesAndReincludesWithNewFiles) {
    test_support::TemporaryDirectory temporary("dbackup-case");
    const std::string root = temporary.string();
    const fs::path base(root), source = base / "source";
    fs::create_directories(source / "excluded/selected");
    std::ofstream(source / "new.txt") << "new";
    std::ofstream(source / "excluded/hidden.txt") << "hidden";
    std::ofstream(source / "excluded/selected/keep.txt") << "keep";
    FilterOptions filter;
    filter.sourceRules = {{source.string(), true},
                          {(source / "excluded").string(), false},
                          {(source / "excluded/selected").string(), true}};
    std::vector<dbackup::archive_scan::Entry> entries;
    BackupPreview preview;
    std::string error;
    ASSERT_TRUE(
        dbackup::archive_scan::collect({source.string()}, {}, filter, entries, &preview, {}, error))
        << error;
    std::set<std::string> paths;
    for (const auto& entry : entries)
        paths.insert(entry.relativePath);
    EXPECT_TRUE(paths.count("new.txt"));
    EXPECT_FALSE(paths.count("excluded/hidden.txt"));
    EXPECT_TRUE(paths.count("excluded/selected/keep.txt"));
    EXPECT_EQ(preview.includedFiles, 2u);
}

namespace {
dbackup::archive_scan::Entry fileEntry() {
    dbackup::archive_scan::Entry entry;
    entry.relativePath = "docs/Report.TXT";
    entry.type = 0;
    entry.owner = "Alice Example";
    entry.metadata.size = 100;
    entry.metadata.mtimeNs = 123456789000000LL;
    entry.metadata.mode = S_IFREG | 0640;
    entry.metadata.links = 1;
    return entry;
}
} // namespace

TEST(BackupPolicyTest, MapsLinuxFileTypesAndHardlinkFilterCategory) {
    using dbackup::backup_policy::fileTypeFromMode;
    EXPECT_EQ(fileTypeFromMode(S_IFREG | 0644), 0);
    EXPECT_EQ(fileTypeFromMode(S_IFDIR | 0755), 1);
    EXPECT_EQ(fileTypeFromMode(S_IFLNK | 0777), 2);
    EXPECT_EQ(fileTypeFromMode(S_IFIFO | 0600), 4);
    EXPECT_EQ(fileTypeFromMode(S_IFCHR | 0600), 5);
    EXPECT_EQ(fileTypeFromMode(S_IFBLK | 0600), 6);
    EXPECT_EQ(fileTypeFromMode(S_IFSOCK | 0600), 7);
    EXPECT_FALSE(fileTypeFromMode(0).has_value());

    EXPECT_EQ(dbackup::backup_policy::filterCategory(0, 1), 0u);
    EXPECT_EQ(dbackup::backup_policy::filterCategory(0, 2), 3u);
    EXPECT_EQ(dbackup::backup_policy::filterCategory(4, 1), 4u);
}

TEST(BackupPolicyTest, MatchesPathTypeNameOwnerExtensionSizeAndTimeRules) {
    auto entry = fileEntry();
    FilterOptions filter;
    filter.typeMask = 1u << 0;
    filter.pathContains = "DOCS";
    filter.nameContains = "report";
    filter.extensions = {"TXT"};
    filter.ownerContains = "alice";
    filter.minSize = 100;
    filter.maxSize = 100;
    filter.afterMtimeMs = 123456789;
    filter.beforeMtimeMs = 123456789;
    EXPECT_TRUE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));

    filter.pathContains = "images";
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.pathContains.clear();
    filter.nameContains = "draft";
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.nameContains.clear();
    filter.extensions = {".png"};
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.extensions.clear();
    filter.ownerContains = "bob";
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.ownerContains.clear();
    filter.minSize = 101;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.minSize = 0;
    filter.maxSize = 99;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.maxSize = 0;
    filter.afterMtimeMs = 123456790;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.afterMtimeMs = 0;
    filter.beforeMtimeMs = 123456788;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
}

TEST(BackupPolicyTest, AppliesTypeMasksAndIncludeExcludePathPatterns) {
    auto entry = fileEntry();
    FilterOptions filter;
    filter.typeMask = 1u << 1;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));

    filter.typeMask = 1u << 0;
    filter.pathRules = {{true, "docs/*"}, {false, "docs/private*"}};
    EXPECT_TRUE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.pathRules.push_back({false, "docs/Report.TXT"});
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));

    filter.pathRules = {{true, "docs/private*"}};
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.pathRules = {{true, "docs/*"}};
    entry.type = 1;
    filter.type = EntryTypeFilter::FilesOnly;
    EXPECT_FALSE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
    filter.type = EntryTypeFilter::DirectoriesOnly;
    filter.typeMask = 1u << 1;
    EXPECT_TRUE(dbackup::backup_policy::matchesFilter(entry, filter, entry.relativePath));
}

TEST(BackupPolicyTest, ResolvesHierarchicalSourceSelectionAndComponentBoundaries) {
    const std::filesystem::path root = "/tmp/dbackup-policy/source";
    const auto rules = std::vector<SourceSelectionRule>{
        {root.string(), true},
        {(root / "excluded").string(), false},
        {(root / "excluded/reincluded").string(), true},
    };

    EXPECT_TRUE(dbackup::backup_policy::sourcePathSelected(root, rules));
    EXPECT_FALSE(dbackup::backup_policy::sourcePathSelected(root / "excluded/item", rules));
    EXPECT_TRUE(
        dbackup::backup_policy::sourcePathSelected(root / "excluded/reincluded/new", rules));
    EXPECT_FALSE(
        dbackup::backup_policy::sourcePathSelected("/tmp/dbackup-policy/source-other", rules));
    EXPECT_TRUE(dbackup::backup_policy::mayContainSelectedSource(root / "excluded", rules));
    EXPECT_FALSE(dbackup::backup_policy::mayContainSelectedSource(
        root / "excluded/reincluded", {
                                          {root.string(), true},
                                          {(root / "excluded").string(), false},
                                      }));
    EXPECT_TRUE(dbackup::backup_policy::hasMixedSourceSelection(root, rules));
    EXPECT_TRUE(dbackup::backup_policy::hasMixedSourceSelection(root / "excluded", rules));
    EXPECT_FALSE(dbackup::backup_policy::hasMixedSourceSelection(root / "plain", rules));
}

TEST(BackupPolicyTest, TogglesSelectionAndReturnsMinimalSelectedRoots) {
    const std::filesystem::path base = "/tmp/dbackup-policy";
    const auto parentSelected = std::vector<SourceSelectionRule>{{base.string(), true}};
    auto rules =
        dbackup::backup_policy::toggleSourceSelection(base / "source", false, parentSelected);
    ASSERT_EQ(rules.size(), 2u);
    EXPECT_FALSE(dbackup::backup_policy::sourcePathSelected(base / "source/file", rules));

    rules = dbackup::backup_policy::toggleSourceSelection(base / "source", true, rules);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_TRUE(dbackup::backup_policy::sourcePathSelected(base / "source/file", rules));

    const auto nested = std::vector<SourceSelectionRule>{
        {(base / "source").string(), true},
        {(base / "source/excluded").string(), false},
        {(base / "source/excluded/reincluded").string(), true},
    };
    const auto roots = dbackup::backup_policy::selectedSourceRoots(nested);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front(), (base / "source").string());

    const std::vector<SourceSelectionRule> noRules;
    EXPECT_FALSE(dbackup::backup_policy::sourcePathSelected(base, noRules, true, false));
    EXPECT_TRUE(dbackup::backup_policy::sourcePathSelected(base, noRules, true, true));
    EXPECT_TRUE(dbackup::backup_policy::toggleSourceSelection(base, true, noRules, false).size() ==
                1u);
}

TEST(BackupPolicyTest, ValidatesMetadataTypeAndDeviceNumberFields) {
    using dbackup::backup_policy::archiveMetadataMatchesType;
    EXPECT_TRUE(archiveMetadataMatchesType(0, S_IFREG | 0644, 0, 0));
    EXPECT_TRUE(archiveMetadataMatchesType(3, S_IFREG | 0644, 0, 0));
    EXPECT_TRUE(archiveMetadataMatchesType(5, S_IFCHR | 0600, 1, 3));
    EXPECT_TRUE(archiveMetadataMatchesType(6, S_IFBLK | 0600, 240, 0));
    EXPECT_TRUE(archiveMetadataMatchesType(7, S_IFSOCK | 0600, 0, 0));
    EXPECT_FALSE(archiveMetadataMatchesType(0, S_IFDIR | 0755, 0, 0));
    EXPECT_TRUE(archiveMetadataMatchesType(5, S_IFCHR | 0600, 0, 0));
    EXPECT_FALSE(archiveMetadataMatchesType(5, S_IFREG | 0600, 1, 3));
    EXPECT_FALSE(archiveMetadataMatchesType(1, S_IFDIR | 0755, 1, 0));
}

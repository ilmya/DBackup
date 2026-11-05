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

TEST(LinuxScan, PreviewCountsSpecialTypesAndFiltersFifo) {
    std::string name=(fs::temp_directory_path()/"dbackup-types-XXXXXX").string();
    const char *root=mkdtemp(name.data());ASSERT_NE(root,nullptr);
    fs::path source=fs::path(root)/"source";fs::create_directory(source);
    std::ofstream(source/"file")<<"data";fs::create_symlink("file",source/"link");
    ASSERT_EQ(mkfifo((source/"pipe").c_str(),0600),0);
    FilterOptions filter;filter.typeMask=1u<<4;BackupPreview preview;std::string error;
    ASSERT_TRUE(ArchiveService::preview({source.string()},filter,preview,error,true))<<error;
    EXPECT_EQ(preview.includedTypes[4],1u);EXPECT_EQ(preview.excludedTypes[0],1u);EXPECT_EQ(preview.excludedTypes[2],1u);
    OperationContext context;context.includeSourceDirectory=true;PackOptions options;options.filter=filter;
    const auto archive=(fs::path(root)/"backup.abk").string();
    ASSERT_TRUE(ArchiveService::create({source.string()},archive,options,error,context))<<error;
    std::vector<ArchiveEntry> entries;ASSERT_TRUE(ArchiveService::inspect(archive,entries,error))<<error;
    std::set<std::string> paths;for(const auto &entry:entries)paths.insert(entry.relativePath);
    EXPECT_TRUE(paths.count("source/pipe"));EXPECT_FALSE(paths.count("source/file"));
    ASSERT_TRUE(ArchiveService::extract(archive,(fs::path(root)/"restore").string(),error))<<error;
    struct stat pipeInfo{};ASSERT_EQ(lstat((fs::path(root)/"restore/source/pipe").c_str(),&pipeInfo),0);
    EXPECT_TRUE(S_ISFIFO(pipeInfo.st_mode));
    fs::remove_all(root);
}

TEST(LinuxScan, PreviewReportsUnreadableFiles) {
    if(geteuid()==0) GTEST_SKIP() << "requires an unprivileged user";
    std::string name=(fs::temp_directory_path()/"dbackup-preview-XXXXXX").string();
    const char *root=mkdtemp(name.data());ASSERT_NE(root,nullptr);
    fs::path file=fs::path(root)/"private";std::ofstream(file)<<"secret";ASSERT_EQ(chmod(file.c_str(),0),0);
    BackupPreview preview;std::string error;
    EXPECT_TRUE(ArchiveService::preview({root},{},preview,error,true));
    EXPECT_EQ(preview.unreadableFiles,1u);EXPECT_NE(error.find("private"),std::string::npos);
    chmod(file.c_str(),0600);fs::remove_all(root);
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

// 验证备份创建、读取、恢复与异常处理
// Copyright 2026 BackupTool Team. All rights reserved

/**
 * test_archive.cpp — archive 模块的 Google Test 单元测试
 *
 * 测试覆盖：
 *   1. 空目录打包/解包
 *   2. 单文件打包/解包（内容一致性校验）
 *   3. 多文件 + 嵌套目录打包/解包
 *   4. 元数据恢复验证（修改时间 mtime）
 *   5. 非法文件校验（不存在的文件、非 .abk 文件）
 *   6. 二进制格式头部校验（Magic 验证）
 */

#include "core/archive/archive.h"
#include "test_support.h"
#include <gtest/gtest.h>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ═══════════════════ 测试辅助 ═══════════════════

static void WriteFile(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), content.size());
}
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static bool FileExists(const std::string& path) {
    return std::filesystem::is_regular_file(path);
}
static bool DirExists(const std::string& path) {
    return std::filesystem::is_directory(path);
}

// ═══════════════════ Test Fixture ═══════════════════

class ArchiveServiceTest : public ::testing::Test {
  protected:
    test_support::TemporaryDirectory temporary{"dbackup-archive"};
    std::string srcDir;
    std::string dstDir;
    std::string abkFile;

    void SetUp() override {
        const std::string base = temporary.string();
        srcDir = base + "/src";
        dstDir = base + "/dst";
        abkFile = base + "/test.abk";
        std::filesystem::create_directory(srcDir);
    }
};

// ═══════════════════ 测试用例 ═══════════════════

/** 测试 1: 空目录打包与解包 */
TEST_F(ArchiveServiceTest, WrappedArchiveCrossesChunkBoundary) {
    const std::string content(4 * 1024 * 1024 + 1, 'x');
    WriteFile(srcDir + "/large.bin", content);
    std::string error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    EXPECT_EQ(test_support::readText(dstDir + "/large.bin"), content);
}

TEST(ArchiveStreaming, AllFormatsAlgorithmsAndPasswordsCrossChunkBoundary) {
    test_support::TemporaryDirectory temporary("dbackup-stream-matrix");
    const auto base = temporary.path();
    const auto source = base / "source";
    std::filesystem::create_directories(source);
    std::string content(4 * 1024 * 1024 + 19, 'a');
    for (size_t i = 0; i < content.size(); i += 127)
        content[i] = static_cast<char>(i % 251);
    test_support::writeText(source / "large.bin", content);
    test_support::writeText(source / "empty", "");
    for (auto format : {PackFormat::Custom, PackFormat::Tar})
        for (auto compression : {Compression::None, Compression::Huffman,
                                 Compression::Lz77, Compression::Deflate})
            for (bool encrypted : {false, true}) {
                PackOptions options;
                options.format = format;
                options.compression = compression;
                options.password = encrypted ? "test-password" : "";
                const auto archive = base / "archive";
                const auto restore = base / "restore";
                std::string error;
                ASSERT_TRUE(ArchiveService::create(source.string(), archive.string(), options, error))
                    << error;
                std::vector<ArchiveEntry> entries;
                ASSERT_TRUE(ArchiveService::inspectMetadata(archive.string(), entries, error,
                                                            options.password)) << error;
                ASSERT_EQ(entries.size(), 2u);
                for (const auto& entry : entries) EXPECT_TRUE(entry.data.empty());
                ASSERT_TRUE(ArchiveService::extract(archive.string(), restore.string(), error,
                                                     options.password)) << error;
                EXPECT_EQ(test_support::readText(restore / "large.bin"), content);
                EXPECT_TRUE(std::filesystem::exists(restore / "empty"));
            }
}

TEST(ArchiveStreaming, MetadataInspectionDoesNotLoadLargeFileContent) {
    test_support::TemporaryDirectory temporary("dbackup-stream-inspect");
    const auto source = temporary.path() / "source";
    const auto archive = temporary.path() / "archive.tar";
    std::filesystem::create_directories(source);
    int fd = open((source / "large").c_str(), O_CREAT | O_WRONLY, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 64LL * 1024 * 1024 + 1), 0);
    close(fd);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    std::string error;
    ASSERT_TRUE(ArchiveService::create(source.string(), archive.string(), options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspectMetadata(archive.string(), entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].originalSize, 64u * 1024u * 1024u + 1u);
    EXPECT_TRUE(entries[0].data.empty());
    EXPECT_FALSE(ArchiveService::inspect(archive.string(), entries, error));
    EXPECT_NE(error.find("64 MiB"), std::string::npos);
}

TEST(ArchiveStreaming, LegacyEnvelopeIsRejectedExplicitly) {
    test_support::TemporaryDirectory temporary("dbackup-old-envelope");
    auto archive = temporary.path() / "old";
    test_support::writeText(archive, "DBPK0001old contents");
    std::string error;
    std::vector<ArchiveEntry> entries;
    EXPECT_FALSE(ArchiveService::inspectMetadata(archive.string(), entries, error));
    EXPECT_NE(error.find("旧版"), std::string::npos);
    EXPECT_FALSE(ArchiveService::extract(archive.string(), (temporary.path() / "out").string(), error));
    EXPECT_NE(error.find("旧版"), std::string::npos);
}

TEST(ArchiveStreaming, SourceChangePreservesExistingArchiveAndCleansTemps) {
    test_support::TemporaryDirectory temporary("dbackup-source-change");
    const auto source = temporary.path() / "source";
    const auto archive = temporary.path() / "archive";
    std::filesystem::create_directories(source);
    test_support::writeText(source / "one", "one");
    test_support::writeText(source / "two", "two");
    test_support::writeText(archive, "existing archive");
    OperationContext context;
    bool changed = false;
    context.onProgress = [&](const OperationProgress& progress) {
        if (!changed && progress.stage == OperationStage::Reading && progress.completedFiles == 1) {
            changed = true;
            const auto other = progress.currentPath == "one" ? source / "two" : source / "one";
            test_support::writeText(other, "changed contents");
        }
    };
    std::string error;
    EXPECT_FALSE(ArchiveService::create({source.string()}, archive.string(), PackOptions{}, error,
                                        context));
    EXPECT_TRUE(changed);
    EXPECT_EQ(test_support::readText(archive), "existing archive");
    for (const auto& item : std::filesystem::directory_iterator(temporary.path()))
        EXPECT_EQ(item.path().filename() == "source" || item.path().filename() == "archive", true);
}

TEST_F(ArchiveServiceTest, EmptyDirectory) {
    std::string error;

    // 打包空目录
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(FileExists(abkFile));

    // 解包
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;

    // 解包后目标目录应为空（没有文件被创建）
    // dstDir 本身会被创建
    ASSERT_TRUE(DirExists(dstDir));
}

TEST_F(ArchiveServiceTest, UnreadableSourceDoesNotReplaceExistingArchive) {
    if (geteuid() == 0)
        GTEST_SKIP() << "root can bypass file permissions";
    const std::string source = srcDir + "/private.txt";
    WriteFile(source, "private content");
    WriteFile(abkFile, "existing archive");
    ASSERT_EQ(chmod(source.c_str(), 0000), 0);
    std::string error;
    const bool success = ArchiveService::create(srcDir, abkFile, error);
    EXPECT_EQ(chmod(source.c_str(), 0600), 0);
    EXPECT_FALSE(success);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ReadFile(abkFile), "existing archive");
}

/** 测试 2: 单文件打包与解包 — 内容一致性 */
TEST_F(ArchiveServiceTest, SingleFileRoundTrip) {
    std::string error;
    std::string content = "Hello, Backup Tool! This is a test file.";
    WriteFile(srcDir + "/hello.txt", content);

    // 打包
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;

    // 解包
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;

    // 验证文件内容完全一致
    std::string restored = ReadFile(dstDir + "/hello.txt");
    ASSERT_EQ(restored, content) << "解包后文件内容不一致";
}

/** 测试 3: 多文件 + 嵌套目录 */
TEST_F(ArchiveServiceTest, MultipleFilesAndDirectories) {
    std::string error;

    // 创建目录结构:
    // src/
    //   file1.txt
    //   subdir/
    //     file2.txt
    //     deep/
    //       file3.txt
    WriteFile(srcDir + "/file1.txt", "content1");
    WriteFile(srcDir + "/subdir/file2.txt", "content2");
    WriteFile(srcDir + "/subdir/deep/file3.txt", "content3");

    // 打包
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;

    // 读取备份验证条目数
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    // 应有: subdir(目录), subdir/deep(目录), file1.txt, subdir/file2.txt, subdir/deep/file3.txt
    ASSERT_EQ(entries.size(), 5u) << "条目数不匹配，实际: " << entries.size();

    // 解包
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;

    // 验证所有文件内容
    ASSERT_EQ(ReadFile(dstDir + "/file1.txt"), "content1");
    ASSERT_EQ(ReadFile(dstDir + "/subdir/file2.txt"), "content2");
    ASSERT_EQ(ReadFile(dstDir + "/subdir/deep/file3.txt"), "content3");

    // 验证目录结构
    ASSERT_TRUE(DirExists(dstDir + "/subdir"));
    ASSERT_TRUE(DirExists(dstDir + "/subdir/deep"));
}

/** 测试 4: 修改时间 (mtime) 恢复验证 */
TEST_F(ArchiveServiceTest, MtimeRestoration) {
    WriteFile(srcDir + "/timed.txt", "time test");
    std::string error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    struct stat info {};
    ASSERT_EQ(stat((dstDir + "/timed.txt").c_str(), &info), 0);
    EXPECT_EQ(info.st_mtim.tv_sec * 1000 + info.st_mtim.tv_nsec / 1000000, entries[0].mtimeMs);
}

/** 测试 5: 非法路径 — 源目录不存在 */
TEST_F(ArchiveServiceTest, PackNonexistentSource) {
    std::string error;
    ASSERT_FALSE(ArchiveService::create("/__nonexistent_dir__", abkFile, error));
    EXPECT_FALSE(error.empty()) << "应返回错误信息";
}

/** 测试 6: 非法文件 — 非 .abk 文件解包 */
TEST_F(ArchiveServiceTest, UnpackInvalidArchive) {
    std::string error;

    // 创建一个假文件
    WriteFile(srcDir + "/fake.abk", "this is not a valid archive");

    ASSERT_FALSE(ArchiveService::extract(srcDir + "/fake.abk", dstDir, error));
    EXPECT_FALSE(error.empty()) << "应返回错误信息";
    EXPECT_FALSE(error.empty());
}

/** 测试 7: 不存在的 .abk 文件解包 */
TEST_F(ArchiveServiceTest, UnpackNonexistentFile) {
    std::string error;
    ASSERT_FALSE(ArchiveService::extract("/__nonexistent__.abk", dstDir, error));
    EXPECT_FALSE(error.empty());
}

/** New archives use the chunked envelope, including the convenience API */
TEST_F(ArchiveServiceTest, ArchiveHeaderFormat) {
    WriteFile(srcDir + "/test.txt", "header test");
    std::string error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    std::ifstream f(abkFile, std::ios::binary);
    char magic[8];
    f.read(magic, 8);
    EXPECT_EQ(std::string(magic, 4), "DBPK");
    EXPECT_EQ(std::string(magic + 4, 4), std::string("\x02\0\0\0", 4));
}

TEST_F(ArchiveServiceTest, RejectsUnsupportedArchiveVersions) {
    for (uint16_t version : {1u, 2u}) {
        std::string bytes("ABKPKG", 6);
        bytes.push_back(static_cast<char>(version & 0xff));
        bytes.push_back(static_cast<char>((version >> 8) & 0xff));
        bytes.append(12, '\0'); // count, flags, and reserved header fields
        const std::string path = srcDir + "/unsupported-v" + std::to_string(version) + ".abk";
        WriteFile(path, bytes);

        std::vector<ArchiveEntry> entries;
        std::string error;
        EXPECT_FALSE(ArchiveService::inspect(path, entries, error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(entries.empty());
    }
}

/** 测试 9: 大文件打包（1MB 数据） */
TEST_F(ArchiveServiceTest, LargeFileRoundTrip) {
    std::string error;

    // 生成 1MB 数据
    std::string largeData(1024 * 1024, '\0');
    for (size_t i = 0; i < largeData.size(); i++) {
        largeData[i] = static_cast<char>(i % 256);
    }
    WriteFile(srcDir + "/large.bin", largeData);

    // 打包
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;

    // 解包
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;

    // 验证内容
    std::string restored = ReadFile(dstDir + "/large.bin");
    ASSERT_EQ(restored.size(), largeData.size()) << "大文件大小不一致";
    ASSERT_EQ(restored, largeData) << "大文件内容不一致";
}

/** 测试 10: 空文件打包 */
TEST_F(ArchiveServiceTest, EmptyFileRoundTrip) {
    std::string error;
    WriteFile(srcDir + "/empty.txt", "");

    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;

    ASSERT_TRUE(FileExists(dstDir + "/empty.txt"));
    ASSERT_EQ(ReadFile(dstDir + "/empty.txt"), "");
}

/** Sprint 2: zlib compression + AES-256 password protection */
TEST_F(ArchiveServiceTest, CompressedEncryptedRoundTrip) {
    std::string error;
    std::string content(256 * 1024, 'A');
    WriteFile(srcDir + "/repetitive.txt", content);

    PackOptions options;
    options.password = "correct horse battery staple";
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error, options.password)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].flags & 1u, 0u) << "重复数据应使用压缩存储";
    ASSERT_EQ(entries[0].data, content);

    std::string wrongError;
    EXPECT_FALSE(ArchiveService::extract(abkFile, dstDir, wrongError, "wrong password"));
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error, options.password)) << error;
    EXPECT_EQ(ReadFile(dstDir + "/repetitive.txt"), content);
}

/** Sprint 2: path/extension/size filters only include matching files */
TEST_F(ArchiveServiceTest, FilterByExtensionAndSize) {
    std::string error;
    WriteFile(srcDir + "/keep.txt", "keep me");
    WriteFile(srcDir + "/skip.bin", "skip me");

    PackOptions options;
    options.filter.type = EntryTypeFilter::FilesOnly;
    options.filter.extension = ".txt";
    options.filter.minSize = 1;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "keep.txt");
}

TEST_F(ArchiveServiceTest, DirectoryMtimeRestoredAfterChildren) {
    WriteFile(srcDir + "/nested/child.txt", "child");
    timespec times[2]{{1700000000, 123456789}, {1700000000, 123456789}};
    ASSERT_EQ(utimensat(AT_FDCWD, (srcDir + "/nested").c_str(), times, 0), 0);
    std::string error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    struct stat value {};
    ASSERT_EQ(stat((dstDir + "/nested").c_str(), &value), 0);
    EXPECT_EQ(value.st_mtim.tv_sec, 1700000000);
    EXPECT_EQ(value.st_mtim.tv_nsec, 123456789);
}

/** Sprint 2: filter expressions reject invalid values instead of silently ignoring them */
TEST(FilterOptionsTest, StrictValidation) {
    FilterOptions filter;
    std::string error;

    ASSERT_TRUE(ParseFilterOptions(" type = file ; ext = .txt ; minsize = 10 ; maxsize = 20 ; "
                                   "after = 2026-01-01 ; before = 2026-01-31 ",
                                   filter, error))
        << error;
    EXPECT_EQ(filter.type, EntryTypeFilter::FilesOnly);
    EXPECT_EQ(filter.extension, ".txt");
    EXPECT_EQ(filter.minSize, 10u);
    EXPECT_EQ(filter.maxSize, 20u);
    EXPECT_LT(filter.afterMtimeMs, filter.beforeMtimeMs);

    EXPECT_FALSE(ParseFilterOptions("minsize=abc", filter, error));
    EXPECT_NE(error.find("minsize"), std::string::npos);
    EXPECT_FALSE(ParseFilterOptions("after=2026-02-30", filter, error));
    EXPECT_FALSE(ParseFilterOptions("type=image", filter, error));
    EXPECT_FALSE(ParseFilterOptions("unknown=value", filter, error));
    EXPECT_FALSE(ParseFilterOptions("minsize=20;maxsize=10", filter, error));
}

TEST(FilterOptionsTest, RejectsInvalidProgrammaticConditions) {
    FilterOptions filter;
    std::string error;
    EXPECT_TRUE(ValidateFilterOptions(filter, error));
    filter.minSize = 20;
    filter.maxSize = 10;
    EXPECT_FALSE(ValidateFilterOptions(filter, error));
    filter = {};
    filter.afterMtimeMs = 20;
    filter.beforeMtimeMs = 10;
    EXPECT_FALSE(ValidateFilterOptions(filter, error));
    filter = {};
    filter.pathRules.push_back({true, ""});
    EXPECT_FALSE(ValidateFilterOptions(filter, error));
}

TEST_F(ArchiveServiceTest, UnicodeAndEmojiFileNamesRoundTrip) {
    const std::string name = u8"emoji-📦-中文.txt";
    WriteFile(srcDir + "/" + name, "unicode path payload");
    std::string error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    EXPECT_EQ(ReadFile(dstDir + "/" + name), "unicode path payload");
}

TEST_F(ArchiveServiceTest, MultipleSourceDirectoryTimesRoundTrip) {
    const std::string first = srcDir + "/first", second = srcDir + "/second";
    std::filesystem::create_directories(first + "/nested");
    std::filesystem::create_directories(second);
    WriteFile(first + "/nested/file.txt", "payload");
    timespec times[2]{{1700000000, 123456789}, {1700000000, 123456789}};
    for (const auto& path : {first + "/nested", first, second})
        ASSERT_EQ(utimensat(AT_FDCWD, path.c_str(), times, 0), 0);
    std::string error;
    ASSERT_TRUE(ArchiveService::create(std::vector<std::string>{first, second}, abkFile, {}, error))
        << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    for (const auto& path : {dstDir + "/first", dstDir + "/first/nested", dstDir + "/second"}) {
        struct stat info {};
        ASSERT_EQ(stat(path.c_str(), &info), 0);
        EXPECT_EQ(info.st_mtim.tv_sec, 1700000000);
        EXPECT_EQ(info.st_mtim.tv_nsec, 123456789);
    }
}

/** Linux file permissions are preserved by the current-format round trip */
TEST_F(ArchiveServiceTest, FileAttributesRestoration) {
    WriteFile(srcDir + "/readonly.txt", "attributes");
    ASSERT_EQ(chmod((srcDir + "/readonly.txt").c_str(), 0440), 0);
    std::string error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    struct stat info {};
    ASSERT_EQ(stat((dstDir + "/readonly.txt").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0440);
}

/** Corruption of an unencrypted current-format archive is detected by SHA-256 */
TEST_F(ArchiveServiceTest, DetectsArchiveTampering) {
    std::string error;
    WriteFile(srcDir + "/data.txt", std::string(4096, 'A'));
    PackOptions options;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;

    std::fstream archive(abkFile, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(archive.is_open());
    archive.seekg(30, std::ios::beg);
    char value = 0;
    archive.read(&value, 1);
    ASSERT_TRUE(archive.good());
    value ^= 0x01;
    archive.seekp(30, std::ios::beg);
    archive.write(&value, 1);
    archive.close();

    std::vector<ArchiveEntry> entries;
    EXPECT_FALSE(ArchiveService::inspect(abkFile, entries, error));
    EXPECT_NE(error.find("校验失败"), std::string::npos);
}

/** Sprint 1/2: replacing an existing archive still produces a valid final file */
TEST_F(ArchiveServiceTest, NoReplacePreservesExistingArchive) {
    WriteFile(srcDir + "/new.txt", "new data");
    WriteFile(abkFile, "existing data");
    PackOptions options;
    OperationContext context;
    context.noReplace = true;
    std::string error;
    EXPECT_FALSE(
        ArchiveService::create(std::vector<std::string>{srcDir}, abkFile, options, error, context));
    EXPECT_EQ(ReadFile(abkFile), "existing data");
    EXPECT_FALSE(error.empty());
}

TEST_F(ArchiveServiceTest, AtomicallyReplacesExistingArchive) {
    std::string error;
    WriteFile(srcDir + "/new.txt", "new data");
    WriteFile(abkFile, "old invalid archive");

    PackOptions options;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].data, "new data");
}

/** The destination archive is excluded when it is saved inside the source directory */
TEST_F(ArchiveServiceTest, ExcludesDestinationArchiveFromSource) {
    std::string error;
    WriteFile(srcDir + "/data.txt", "data");
    abkFile = srcDir + "/inside.abk";
    PackOptions options;

    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "data.txt");
}

/** A single file can be selected as the complete backup source */
TEST_F(ArchiveServiceTest, SingleFileAsBackupSource) {
    std::string error;
    const std::string sourceFile = srcDir + "/selected.txt";
    WriteFile(sourceFile, "selected file only");
    PackOptions options;

    ASSERT_TRUE(ArchiveService::create(sourceFile, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "selected.txt");
    EXPECT_EQ(entries[0].data, "selected file only");

    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    EXPECT_EQ(ReadFile(dstDir + "/selected.txt"), "selected file only");
}

/** The source file and output archive must not be the same path */
TEST_F(ArchiveServiceTest, RejectsSourceEqualToDestination) {
    std::string error;
    const std::string sourceFile = srcDir + "/same.abk";
    WriteFile(sourceFile, "do not overwrite");
    PackOptions options;

    EXPECT_FALSE(ArchiveService::create(sourceFile, sourceFile, options, error));
    EXPECT_NE(error.find("不能同时"), std::string::npos);
    EXPECT_EQ(ReadFile(sourceFile), "do not overwrite");
}

/** Multiple files and folders can be combined into one archive */
TEST_F(ArchiveServiceTest, MultipleBackupSources) {
    std::string error;
    const std::string first = srcDir + "/first.txt";
    const std::string second = srcDir + "/second.bin";
    const std::string folder = srcDir + "/folder";
    WriteFile(first, "first");
    WriteFile(second, "second");
    WriteFile(folder + "/nested.txt", "nested");
    PackOptions options;

    ASSERT_TRUE(ArchiveService::create(std::vector<std::string>{first, second, folder}, abkFile,
                                       options, error))
        << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 4u); // two files, the selected folder metadata, and its nested file

    ASSERT_TRUE(ArchiveService::extract(abkFile, dstDir, error)) << error;
    EXPECT_EQ(ReadFile(dstDir + "/first.txt"), "first");
    EXPECT_EQ(ReadFile(dstDir + "/second.bin"), "second");
    EXPECT_EQ(ReadFile(dstDir + "/folder/nested.txt"), "nested");
}

/** Path, type, name, time, size and owner filters work together */
TEST_F(ArchiveServiceTest, FilterAcrossAllSixCategories) {
    std::string error;
    WriteFile(srcDir + "/docs/report.txt", "filter target");
    WriteFile(srcDir + "/docs/other.bin", "other");

    PackOptions baselineOptions;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, baselineOptions, error)) << error;
    std::vector<ArchiveEntry> baseline;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, baseline, error)) << error;
    auto target = std::find_if(baseline.begin(), baseline.end(), [](const ArchiveEntry& entry) {
        return entry.relativePath == "docs/report.txt";
    });
    ASSERT_NE(target, baseline.end());
    ASSERT_FALSE(target->owner.empty()) << "当前系统应能读取测试文件所有者";

    PackOptions options;
    options.filter.pathContains = "docs";
    options.filter.type = EntryTypeFilter::FilesOnly;
    options.filter.nameContains = "report";
    options.filter.afterMtimeMs = target->mtimeMs - 1000;
    options.filter.beforeMtimeMs = target->mtimeMs + 1000;
    options.filter.minSize = 1;
    options.filter.maxSize = 1024;
    options.filter.ownerContains = target->owner;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> filtered;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, filtered, error)) << error;
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].relativePath, "docs/report.txt");
}

/** Type presets, wildcard exclusions and metadata-only preview agree with packing */
TEST_F(ArchiveServiceTest, PreviewWithMultiExtensionAndWildcardRules) {
    std::string error;
    WriteFile(srcDir + "/report.pdf", "document");
    WriteFile(srcDir + "/photo.jpg", "image");
    WriteFile(srcDir + "/cache.tmp", "temporary");

    FilterOptions filter;
    filter.extensions = {".pdf", ".jpg"};
    filter.pathRules.push_back(PathFilterRule{false, "*.jpg"});

    BackupPreview preview;
    ASSERT_TRUE(ArchiveService::preview({srcDir}, filter, preview, error)) << error;
    EXPECT_EQ(preview.includedFiles, 1u);
    EXPECT_EQ(preview.excludedFiles, 2u);
    EXPECT_EQ(preview.includedBytes, 8u);

    PackOptions options;
    options.filter = filter;
    ASSERT_TRUE(ArchiveService::create(srcDir, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspect(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "report.pdf");
}

TEST(LinuxArchive, EncryptedRoundTripWithLinksAndFilter) {
    test_support::TemporaryDirectory temporary("dbackup-archive");
    fs::path base = temporary.path(), source = base / "source";
    fs::create_directories(source / "empty");
    std::ofstream(source / "文件.txt") << "backup content";
    ASSERT_EQ(chmod((source / "文件.txt").c_str(), 0640), 0);
    fs::create_hard_link(source / "文件.txt", source / "hard.txt");
    fs::create_symlink("文件.txt", source / "link");
    PackOptions options;
    options.password = "correct";
    std::string error;
    ASSERT_TRUE(
        ArchiveService::create(source.string(), (base / "backup.abk").string(), options, error))
        << error;
    EXPECT_FALSE(ArchiveService::extract((base / "backup.abk").string(), (base / "wrong").string(),
                                         error, "wrong"));
    ASSERT_TRUE(ArchiveService::extract((base / "backup.abk").string(),
                                        (base / "restored").string(), error, "correct"))
        << error;
    EXPECT_TRUE(fs::is_directory(base / "restored/empty"));
    EXPECT_EQ(fs::read_symlink(base / "restored/link"), "文件.txt");
    struct stat first {
    }, second{};
    ASSERT_EQ(stat((base / "restored/文件.txt").c_str(), &first), 0);
    ASSERT_EQ(stat((base / "restored/hard.txt").c_str(), &second), 0);
    EXPECT_EQ(first.st_ino, second.st_ino);
    EXPECT_EQ(first.st_mode & 0777, 0640u);
    std::ifstream content(base / "restored/文件.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(content), {}), "backup content");
}

TEST(ArchiveV3Test, UsesAuthenticatedEncryptionAndSupportsCancellation) {
    test_support::TemporaryDirectory temporary("dbackup-authenticated-archive");
    const auto base = temporary.path();
    const std::string source = (base / "source").string();
    const std::string archive = (base / "backup.abk").string();
    const std::string restore = (base / "restore").string();
    std::filesystem::create_directory(source);
    test_support::writeText(source + "/secret.txt", "authenticated archive");
    PackOptions options;
    options.password = "correct";
    std::string error;
    ASSERT_TRUE(ArchiveService::create(source, archive, options, error)) << error;
    std::ifstream input(archive, std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
    ASSERT_GT(bytes.size(), 50u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "DBPK");
    EXPECT_EQ(std::vector<unsigned char>(bytes.begin() + 4, bytes.begin() + 8),
              (std::vector<unsigned char>{2, 0, 0, 0}));
    bytes.back() ^= 0x01;
    std::ofstream changed(archive, std::ios::binary | std::ios::trunc);
    changed.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
    changed.close();
    EXPECT_FALSE(ArchiveService::extract(archive, restore, error, "correct"));
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(
        ArchiveService::create(std::vector<std::string>{source}, archive, options, error, context));
}

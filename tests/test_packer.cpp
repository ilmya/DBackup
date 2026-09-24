// Copyright 2026 BackupTool Team. All rights reserved.

/**
 * test_packer.cpp — packer 模块的 Google Test 单元测试
 *
 * 测试覆盖：
 *   1. 空目录打包/解包
 *   2. 单文件打包/解包（内容一致性校验）
 *   3. 多文件 + 嵌套目录打包/解包
 *   4. 元数据恢复验证（修改时间 mtime）
 *   5. 非法文件校验（不存在的文件、非 .abk 文件）
 *   6. 二进制格式头部校验（Magic 验证）
 */

#include <gtest/gtest.h>
#include "packer.h"

#include <windows.h>
#include <fstream>
#include <string>
#include <vector>

// ═══════════════════ 测试辅助 ═══════════════════

static int g_testCounter = 0;

/** 创建带唯一名称的临时测试目录 */
static std::string CreateTestDir() {
    g_testCounter++;
    std::string dir = "test_output/t" + std::to_string(g_testCounter) +
                      "_" + std::to_string(GetCurrentProcessId());
    // 递归创建
    std::string cmd = "mkdir \"" + dir + "\" 2>nul";
    system(cmd.c_str());
    // 也创建子路径中可能缺少的部分
    CreateDirectoryA("test_output", nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

/** 递归删除目录 */
static void RemoveTestDir(const std::string &dir) {
    std::string cmd = "rmdir /s /q \"" + dir + "\" 2>nul";
    system(cmd.c_str());
}

/** 写入文本文件 */
static void WriteFile(const std::string &path, const std::string &content) {
    // 确保父目录存在
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        std::string cmd = "mkdir \"" + parent + "\" 2>nul";
        system(cmd.c_str());
    }
    std::ofstream f(path, std::ios::binary);
    f.write(content.c_str(), content.size());
    f.close();
}

/** 读取文件内容 */
static std::string ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();
    return data;
}

/** 检查文件是否存在 */
static bool FileExists(const std::string &path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/** 检查目录是否存在 */
static bool DirExists(const std::string &path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ═══════════════════ Test Fixture ═══════════════════

class PackerTest : public ::testing::Test {
 protected:
    std::string srcDir;    // 源目录
    std::string dstDir;    // 解包目标目录
    std::string abkFile;   // .abk 文件路径

    void SetUp() override {
        std::string base = CreateTestDir();
        srcDir  = base + "\\src";
        dstDir  = base + "\\dst";
        abkFile = base + "\\test.abk";
        CreateDirectoryA(srcDir.c_str(), nullptr);
    }

    void TearDown() override {
        // 从 abkFile 路径中提取 base 目录
        size_t pos = abkFile.find_last_of("\\/");
        if (pos != std::string::npos) {
            RemoveTestDir(abkFile.substr(0, pos));
        }
    }
};

// ═══════════════════ 测试用例 ═══════════════════

/** 测试 1: 空目录打包与解包 */
TEST_F(PackerTest, EmptyDirectory) {
    std::string error;

    // 打包空目录
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(FileExists(abkFile));

    // 解包
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    // 解包后目标目录应为空（没有文件被创建）
    // dstDir 本身会被创建
    ASSERT_TRUE(DirExists(dstDir));
}

/** 测试 2: 单文件打包与解包 — 内容一致性 */
TEST_F(PackerTest, SingleFileRoundTrip) {
    std::string error;
    std::string content = "Hello, Backup Tool! This is a test file.";
    WriteFile(srcDir + "\\hello.txt", content);

    // 打包
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;

    // 解包
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    // 验证文件内容完全一致
    std::string restored = ReadFile(dstDir + "\\hello.txt");
    ASSERT_EQ(restored, content) << "解包后文件内容不一致";
}

/** 测试 3: 多文件 + 嵌套目录 */
TEST_F(PackerTest, MultipleFilesAndDirectories) {
    std::string error;

    // 创建目录结构:
    // src/
    //   file1.txt
    //   subdir/
    //     file2.txt
    //     deep/
    //       file3.txt
    WriteFile(srcDir + "\\file1.txt", "content1");
    WriteFile(srcDir + "\\subdir\\file2.txt", "content2");
    WriteFile(srcDir + "\\subdir\\deep\\file3.txt", "content3");

    // 打包
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;

    // 读取归档验证条目数
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    // 应有: subdir(目录), subdir/deep(目录), file1.txt, subdir/file2.txt, subdir/deep/file3.txt
    ASSERT_EQ(entries.size(), 5u) << "条目数不匹配，实际: " << entries.size();

    // 解包
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    // 验证所有文件内容
    ASSERT_EQ(ReadFile(dstDir + "\\file1.txt"), "content1");
    ASSERT_EQ(ReadFile(dstDir + "\\subdir\\file2.txt"), "content2");
    ASSERT_EQ(ReadFile(dstDir + "\\subdir\\deep\\file3.txt"), "content3");

    // 验证目录结构
    ASSERT_TRUE(DirExists(dstDir + "\\subdir"));
    ASSERT_TRUE(DirExists(dstDir + "\\subdir\\deep"));
}

/** 测试 4: 修改时间 (mtime) 恢复验证 */
TEST_F(PackerTest, MtimeRestoration) {
    std::string error;
    WriteFile(srcDir + "\\timed.txt", "time test");

    // 打包
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;

    // 读取归档获取记录的 mtime
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    int64_t recordedMtime = entries[0].mtimeMs;

    // 解包
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    // 验证解包后文件的 mtime 与记录值一致（允许 1 秒误差，因文件系统精度限制）
    HANDLE hFile = CreateFileA(
        (dstDir + "\\timed.txt").c_str(),
        FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(hFile, INVALID_HANDLE_VALUE);

    FILETIME ft;
    GetFileTime(hFile, nullptr, nullptr, &ft);
    CloseHandle(hFile);

    ULARGE_INTEGER ul;
    ul.LowPart  = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    int64_t restoredMs = (static_cast<int64_t>(ul.QuadPart) - 116444736000000000LL) / 10000;

    // 允许 2 秒误差（Windows 文件系统精度约 100ns，但转换可能有微小偏差）
    EXPECT_NEAR(static_cast<double>(restoredMs), static_cast<double>(recordedMtime), 2000.0)
        << "mtime 恢复偏差过大";
}

/** 测试 5: 非法路径 — 源目录不存在 */
TEST_F(PackerTest, PackNonexistentSource) {
    std::string error;
    ASSERT_FALSE(Packer::pack("C:\\__nonexistent_dir__", abkFile, error));
    EXPECT_FALSE(error.empty()) << "应返回错误信息";
}

/** 测试 6: 非法文件 — 非 .abk 文件解包 */
TEST_F(PackerTest, UnpackInvalidArchive) {
    std::string error;

    // 创建一个假文件
    WriteFile(srcDir + "\\fake.abk", "this is not a valid archive");

    ASSERT_FALSE(Packer::unpack(srcDir + "\\fake.abk", dstDir, error));
    EXPECT_FALSE(error.empty()) << "应返回错误信息";
    EXPECT_NE(error.find("Magic"), std::string::npos) << "应提示 Magic 不匹配";
}

/** 测试 7: 不存在的 .abk 文件解包 */
TEST_F(PackerTest, UnpackNonexistentFile) {
    std::string error;
    ASSERT_FALSE(Packer::unpack("C:\\__nonexistent__.abk", dstDir, error));
    EXPECT_FALSE(error.empty());
}

/** 测试 8: 二进制格式头部校验 */
TEST_F(PackerTest, ArchiveHeaderFormat) {
    std::string error;
    WriteFile(srcDir + "\\test.txt", "header test");

    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;

    // 读取 .abk 文件的前 20 字节，验证头部格式
    std::ifstream f(abkFile, std::ios::binary);
    ASSERT_TRUE(f.is_open());

    // Magic: "ABKPKG"
    char magic[6];
    f.read(magic, 6);
    EXPECT_EQ(std::string(magic, 6), "ABKPKG") << "Magic 不匹配";

    // Version: uint16 = 1
    uint16_t version;
    f.read(reinterpret_cast<char*>(&version), 2);
    EXPECT_EQ(version, 1u) << "版本号应为 1";

    // EntryCount: uint32 = 1 (只有一个文件)
    uint32_t count;
    f.read(reinterpret_cast<char*>(&count), 4);
    EXPECT_EQ(count, 1u) << "条目数应为 1";

    f.close();
}

/** 测试 9: 大文件打包（1MB 数据） */
TEST_F(PackerTest, LargeFileRoundTrip) {
    std::string error;

    // 生成 1MB 数据
    std::string largeData(1024 * 1024, '\0');
    for (size_t i = 0; i < largeData.size(); i++) {
        largeData[i] = static_cast<char>(i % 256);
    }
    WriteFile(srcDir + "\\large.bin", largeData);

    // 打包
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;

    // 解包
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    // 验证内容
    std::string restored = ReadFile(dstDir + "\\large.bin");
    ASSERT_EQ(restored.size(), largeData.size()) << "大文件大小不一致";
    ASSERT_EQ(restored, largeData) << "大文件内容不一致";
}

/** 测试 10: 空文件打包 */
TEST_F(PackerTest, EmptyFileRoundTrip) {
    std::string error;
    WriteFile(srcDir + "\\empty.txt", "");

    ASSERT_TRUE(Packer::pack(srcDir, abkFile, error)) << error;
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    ASSERT_TRUE(FileExists(dstDir + "\\empty.txt"));
    ASSERT_EQ(ReadFile(dstDir + "\\empty.txt"), "");
}

/** Sprint 2: zlib compression + AES-256 password protection. */
TEST_F(PackerTest, CompressedEncryptedRoundTrip) {
    std::string error;
    std::string content(256 * 1024, 'A');
    WriteFile(srcDir + "\\repetitive.txt", content);

    PackOptions options;
    options.password = "correct horse battery staple";
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error, options.password)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].flags & 1u, 0u) << "重复数据应使用压缩存储";
    ASSERT_EQ(entries[0].data, content);

    std::string wrongError;
    EXPECT_FALSE(Packer::unpack(abkFile, dstDir, wrongError, "wrong password"));
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error, options.password)) << error;
    EXPECT_EQ(ReadFile(dstDir + "\\repetitive.txt"), content);
}

/** Sprint 2: path/extension/size filters only include matching files. */
TEST_F(PackerTest, FilterByExtensionAndSize) {
    std::string error;
    WriteFile(srcDir + "\\keep.txt", "keep me");
    WriteFile(srcDir + "\\skip.bin", "skip me");

    PackOptions options;
    options.filter.type = EntryTypeFilter::FilesOnly;
    options.filter.extension = ".txt";
    options.filter.minSize = 1;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "keep.txt");
}

/** Sprint 2: filter expressions reject invalid values instead of silently ignoring them. */
TEST(FilterOptionsTest, StrictValidation) {
    FilterOptions filter;
    std::string error;

    ASSERT_TRUE(ParseFilterOptions(
        " type = file ; ext = .txt ; minsize = 10 ; maxsize = 20 ; "
        "after = 2026-01-01 ; before = 2026-01-31 ", filter, error)) << error;
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

/** Sprint 1: Windows file attributes are preserved by the v2 round trip. */
TEST_F(PackerTest, FileAttributesRestoration) {
    std::string error;
    const std::string source = srcDir + "\\readonly.txt";
    WriteFile(source, "attributes");
    ASSERT_TRUE(SetFileAttributesA(source.c_str(), FILE_ATTRIBUTE_READONLY));

    PackOptions options;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;
    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;

    DWORD attributes = GetFileAttributesA((dstDir + "\\readonly.txt").c_str());
    ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_NE(attributes & FILE_ATTRIBUTE_READONLY, 0u);

    // Allow the fixture cleanup to remove both files.
    SetFileAttributesA(source.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesA((dstDir + "\\readonly.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

/** Sprint 2: corruption of an unencrypted v2 archive is detected by SHA-256. */
TEST_F(PackerTest, DetectsArchiveTampering) {
    std::string error;
    WriteFile(srcDir + "\\data.txt", std::string(4096, 'A'));
    PackOptions options;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;

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
    EXPECT_FALSE(Packer::readArchive(abkFile, entries, error));
    EXPECT_NE(error.find("校验失败"), std::string::npos);
}

/** Sprint 1/2: replacing an existing archive still produces a valid final file. */
TEST_F(PackerTest, AtomicallyReplacesExistingArchive) {
    std::string error;
    WriteFile(srcDir + "\\new.txt", "new data");
    WriteFile(abkFile, "old invalid archive");

    PackOptions options;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].data, "new data");
}

/** The destination archive is excluded when it is saved inside the source directory. */
TEST_F(PackerTest, ExcludesDestinationArchiveFromSource) {
    std::string error;
    WriteFile(srcDir + "\\data.txt", "data");
    abkFile = srcDir + "\\inside.abk";
    PackOptions options;

    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "data.txt");
}

/** A single file can be selected as the complete backup source. */
TEST_F(PackerTest, SingleFileAsBackupSource) {
    std::string error;
    const std::string sourceFile = srcDir + "\\selected.txt";
    WriteFile(sourceFile, "selected file only");
    PackOptions options;

    ASSERT_TRUE(Packer::pack(sourceFile, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "selected.txt");
    EXPECT_EQ(entries[0].data, "selected file only");

    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;
    EXPECT_EQ(ReadFile(dstDir + "\\selected.txt"), "selected file only");
}

/** The source file and output archive must not be the same path. */
TEST_F(PackerTest, RejectsSourceEqualToDestination) {
    std::string error;
    const std::string sourceFile = srcDir + "\\same.abk";
    WriteFile(sourceFile, "do not overwrite");
    PackOptions options;

    EXPECT_FALSE(Packer::pack(sourceFile, sourceFile, options, error));
    EXPECT_NE(error.find("不能同时"), std::string::npos);
    EXPECT_EQ(ReadFile(sourceFile), "do not overwrite");
}

/** Multiple files and folders can be combined into one archive. */
TEST_F(PackerTest, MultipleBackupSources) {
    std::string error;
    const std::string first = srcDir + "\\first.txt";
    const std::string second = srcDir + "\\second.bin";
    const std::string folder = srcDir + "\\folder";
    WriteFile(first, "first");
    WriteFile(second, "second");
    WriteFile(folder + "\\nested.txt", "nested");
    PackOptions options;

    ASSERT_TRUE(Packer::pack(std::vector<std::string>{first, second, folder},
                             abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 3u);  // two files and one nested file (parent is created on restore)

    ASSERT_TRUE(Packer::unpack(abkFile, dstDir, error)) << error;
    EXPECT_EQ(ReadFile(dstDir + "\\first.txt"), "first");
    EXPECT_EQ(ReadFile(dstDir + "\\second.bin"), "second");
    EXPECT_EQ(ReadFile(dstDir + "\\folder\\nested.txt"), "nested");
}

/** Path, type, name, time, size and owner filters work together. */
TEST_F(PackerTest, FilterAcrossAllSixCategories) {
    std::string error;
    WriteFile(srcDir + "\\docs\\report.txt", "filter target");
    WriteFile(srcDir + "\\docs\\other.bin", "other");

    PackOptions baselineOptions;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, baselineOptions, error)) << error;
    std::vector<ArchiveEntry> baseline;
    ASSERT_TRUE(Packer::readArchive(abkFile, baseline, error)) << error;
    auto target = std::find_if(baseline.begin(), baseline.end(), [](const ArchiveEntry &entry) {
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
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;

    std::vector<ArchiveEntry> filtered;
    ASSERT_TRUE(Packer::readArchive(abkFile, filtered, error)) << error;
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].relativePath, "docs/report.txt");
}

/** Type presets, wildcard exclusions and metadata-only preview agree with packing. */
TEST_F(PackerTest, PreviewWithMultiExtensionAndWildcardRules) {
    std::string error;
    WriteFile(srcDir + "\\report.pdf", "document");
    WriteFile(srcDir + "\\photo.jpg", "image");
    WriteFile(srcDir + "\\cache.tmp", "temporary");

    FilterOptions filter;
    filter.extensions = {".pdf", ".jpg"};
    filter.pathRules.push_back(PathFilterRule{false, "*.jpg"});

    BackupPreview preview;
    ASSERT_TRUE(Packer::preview({srcDir}, filter, preview, error)) << error;
    EXPECT_EQ(preview.includedFiles, 1u);
    EXPECT_EQ(preview.excludedFiles, 2u);
    EXPECT_EQ(preview.includedBytes, 8u);

    PackOptions options;
    options.filter = filter;
    ASSERT_TRUE(Packer::pack(srcDir, abkFile, options, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(Packer::readArchive(abkFile, entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relativePath, "report.pdf");
}

// ═══════════════════ main ═══════════════════

int main(int argc, char **argv) {
    // 确保 test_output 目录存在
    CreateDirectoryA("test_output", nullptr);

    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();

    // 清理 test_output
    system("rmdir /s /q test_output 2>nul");

    return result;
}

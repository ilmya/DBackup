// 验证tar 格式兼容性及安全规则
#include "core/archive/archive_tar.h"
#include <gtest/gtest.h>

TEST(TarRules, LargeFileHeaderUsesPaxSize) {
    dbackup::archive_format::Entry entry;
    entry.file.relativePath = "large";
    entry.file.type = 0;
    std::vector<uint8_t> headers;
    std::string error;
    ASSERT_TRUE(dbackup::archive_tar::encodeEntryHeaders(entry, 1ULL << 33, 0, headers, error))
        << error;
    const std::string text(headers.begin(), headers.end());
    EXPECT_NE(text.find("size=8589934592\n"), std::string::npos);
}

TEST(TarRules, RejectsEmptyAndTruncatedInput) {
    std::vector<dbackup::archive_format::Entry> entries;
    std::string error;
    EXPECT_FALSE(dbackup::archive_tar::decode({}, entries, error));
    EXPECT_FALSE(dbackup::archive_tar::decode(std::vector<uint8_t>(512, 0), entries, error));
}
TEST(TarRules, AcceptsEmptyArchive) {
    std::vector<dbackup::archive_format::Entry> entries;
    std::string error;
    EXPECT_TRUE(dbackup::archive_tar::decode(std::vector<uint8_t>(1024, 0), entries, error));
    EXPECT_TRUE(entries.empty());
}
TEST(TarRules, RejectsInvalidChecksum) {
    std::vector<dbackup::archive_format::Entry> entries;
    std::string error;
    std::vector<uint8_t> data(1024, 0);
    data[0] = 'x';
    EXPECT_FALSE(dbackup::archive_tar::decode(data, entries, error));
}
TEST(TarRules, RejectsUnsafePathsAndUnsupportedTypes) {
    dbackup::archive_format::Entry entry;
    std::vector<uint8_t> data;
    std::string error;
    for (const char* path : {"../escape", "/absolute", ""}) {
        entry.file.relativePath = path;
        EXPECT_FALSE(dbackup::archive_tar::encode({entry}, data, error));
    }
    entry.file.relativePath = "valid";
    entry.file.type = 8;
    EXPECT_FALSE(dbackup::archive_tar::encode({entry}, data, error));
}

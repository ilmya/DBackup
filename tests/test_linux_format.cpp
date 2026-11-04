#include "core/archive_internal.h"
#include <gtest/gtest.h>

TEST(LinuxArchiveFormat, EncryptedCompressedRoundTripAndCorruption) {
    dbackup::archive_format::Entry entry;
    entry.file.relativePath = "文件.txt";
    entry.file.metadata.mode = 0100640;
    entry.file.metadata.uid = 1000;
    entry.file.metadata.gid = 1001;
    entry.file.metadata.mtimeNs = 123456789123LL;
    entry.data.assign(4096, 'x');
    std::vector<uint8_t> encoded;
    std::string error;
    ASSERT_TRUE(dbackup::archive_format::encode({entry}, 6, "password", encoded, error)) << error;
    EXPECT_LT(encoded.size(), entry.data.size());
    std::vector<dbackup::archive_format::Entry> decoded;
    EXPECT_FALSE(dbackup::archive_format::decode(encoded, "wrong", decoded, error));
    ASSERT_TRUE(dbackup::archive_format::decode(encoded, "password", decoded, error)) << error;
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0].file.relativePath, entry.file.relativePath);
    EXPECT_EQ(decoded[0].file.metadata.mode, entry.file.metadata.mode);
    EXPECT_EQ(decoded[0].file.metadata.uid, entry.file.metadata.uid);
    EXPECT_EQ(decoded[0].file.metadata.gid, entry.file.metadata.gid);
    EXPECT_EQ(decoded[0].file.metadata.mtimeNs, entry.file.metadata.mtimeNs);
    EXPECT_EQ(decoded[0].data, entry.data);
    encoded.back() ^= 1;
    EXPECT_FALSE(dbackup::archive_format::decode(encoded, "password", decoded, error));
}

TEST(LinuxArchiveFormat, RejectsOldAndInvalidPaths) {
    std::vector<dbackup::archive_format::Entry> decoded;
    std::vector<uint8_t> encoded{'A', 'B', 'K', 'P', 'K', 'G', 3, 0};
    std::string error;
    EXPECT_FALSE(dbackup::archive_format::decode(encoded, "", decoded, error));
    dbackup::archive_format::Entry unsafe;
    unsafe.file.relativePath = "../escape";
    EXPECT_FALSE(dbackup::archive_format::encode({unsafe}, 6, "", encoded, error));
}

// 验证备份流式处理
#include "core/archive/archive_internal.h"
#include "core/archive/archive_blocks.h"
#include "core/archive/archive_tar.h"
#include "core/compression/compression.h"
#include "core/security/crypto.h"
#include "core/security/md5.h"
#include "test_support.h"
#include <algorithm>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <random>
#include <unistd.h>
#include <zlib.h>

TEST(ArchiveBlocks, RoundTripAndAuthenticatedMetadata) {
    test_support::TemporaryDirectory temporary("dbackup-block-test");
    auto source = temporary.path() / "source";
    auto archive = temporary.path() / "archive";
    auto output = temporary.path() / "output";
    std::string contents(4 * 1024 * 1024 + 29, 'a');
    for (size_t i = 0; i < contents.size(); i += 97)
        contents[i] = static_cast<char>(i % 251);
    test_support::writeText(source, contents);
    for (auto method : {Compression::None, Compression::Huffman, Compression::Lz77,
                        Compression::Deflate}) {
        for (bool encrypted : {false, true}) {
            int in = open(source.c_str(), O_RDONLY);
            int out = open(archive.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
            ASSERT_GE(in, 0);
            ASSERT_GE(out, 0);
            PackOptions options;
            options.compression = method;
            options.password = encrypted ? "test-password" : "";
            std::string error;
            ASSERT_TRUE(dbackup::archive_blocks::encode(in, out, options, {}, error)) << error;
            close(in);
            ASSERT_EQ(lseek(out, 0, SEEK_SET), 0);
            int restored = open(output.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
            ASSERT_GE(restored, 0);
            PackFormat format;
            ASSERT_TRUE(dbackup::archive_blocks::decode(out, restored, options.password, format, {}, error)) << error;
            EXPECT_EQ(format, PackFormat::Custom);
            close(restored);
            EXPECT_EQ(test_support::readText(output), contents);
            if (encrypted) {
                ASSERT_EQ(lseek(out, 0, SEEK_SET), 0);
                restored = open(output.c_str(), O_TRUNC | O_RDWR);
                ASSERT_GE(restored, 0);
                EXPECT_FALSE(dbackup::archive_blocks::decode(out, restored, "wrong", format, {}, error));
                close(restored);
                unsigned char changed = 1;
                ASSERT_EQ(pwrite(out, &changed, 1, 45), 1); // 原始长度参与认证
                ASSERT_EQ(lseek(out, 0, SEEK_SET), 0);
                restored = open(output.c_str(), O_TRUNC | O_RDWR);
                ASSERT_GE(restored, 0);
                EXPECT_FALSE(dbackup::archive_blocks::decode(out, restored, options.password, format, {}, error));
                close(restored);
            }
            close(out);
        }
    }
}

TEST(ArchiveBlocks, RejectsTruncatedReorderedAndTamperedRecords) {
    test_support::TemporaryDirectory temporary("dbackup-block-corrupt");
    const auto source = temporary.path() / "source";
    const auto archive = temporary.path() / "archive";
    const auto restoredPath = temporary.path() / "restored";
    test_support::writeText(source, "authenticated payload");
    const int input = open(source.c_str(), O_RDONLY);
    const int output = open(archive.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    ASSERT_GE(input, 0);
    ASSERT_GE(output, 0);
    PackOptions options;
    options.compression = Compression::None;
    options.password = "password";
    std::string error;
    ASSERT_TRUE(dbackup::archive_blocks::encode(input, output, options, {}, error)) << error;
    close(input);
    close(output);
    const std::string good = test_support::readText(archive);
    ASSERT_GT(good.size(), 100u);
    auto rejected = [&](const std::string& bytes) {
        test_support::writeText(archive, bytes);
        int in = open(archive.c_str(), O_RDONLY);
        int out = open(restoredPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (in < 0 || out < 0) {
            if (in >= 0) close(in);
            if (out >= 0) close(out);
            return false;
        }
        PackFormat format;
        std::string detail;
        bool ok = dbackup::archive_blocks::decode(in, out, options.password, format, {}, detail);
        close(in);
        close(out);
        return !ok;
    };
    auto changed = good;
    changed[41] ^= 1; // 修改第一个数据块的序号
    EXPECT_TRUE(rejected(changed));
    changed = good;
    changed[53] ^= 1; // 修改第一个加密数据字节
    EXPECT_TRUE(rejected(changed));
    changed = good;
    changed.back() ^= 1; // 修改结束记录的认证标签
    EXPECT_TRUE(rejected(changed));
    EXPECT_TRUE(rejected(good.substr(0, good.size() - 1)));
    EXPECT_TRUE(rejected(good + "x"));
}

TEST(ArchiveBlocks, RejectsDeletedDuplicatedAndReorderedBlocks) {
    test_support::TemporaryDirectory temporary("dbackup-block-order");
    auto inputPath = temporary.path() / "input";
    auto archivePath = temporary.path() / "archive";
    auto outputPath = temporary.path() / "output";
    test_support::writeText(inputPath, std::string(4 * 1024 * 1024 + 1, 'z'));
    int in = open(inputPath.c_str(), O_RDONLY);
    int out = open(archivePath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_GE(in, 0);
    ASSERT_GE(out, 0);
    PackOptions options;
    options.compression = Compression::None;
    options.password = "password";
    std::string error;
    ASSERT_TRUE(dbackup::archive_blocks::encode(in, out, options, {}, error)) << error;
    close(in);
    close(out);
    auto bytes = test_support::readText(archivePath);
    const size_t first = 40, second = first + 13 + 4 * 1024 * 1024 + 16;
    const size_t footer = second + 13 + 1 + 16;
    ASSERT_LT(footer, bytes.size());
    auto fails = [&](const std::string& modified) {
        test_support::writeText(archivePath, modified);
        int source = open(archivePath.c_str(), O_RDONLY);
        int target = open(outputPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (source < 0 || target < 0) {
            if (source >= 0) close(source);
            if (target >= 0) close(target);
            return false;
        }
        PackFormat format;
        std::string detail;
        bool result = dbackup::archive_blocks::decode(source, target, options.password, format, {}, detail);
        close(source);
        close(target);
        return !result;
    };
    const auto head = bytes.substr(0, first);
    const auto a = bytes.substr(first, second - first);
    const auto b = bytes.substr(second, footer - second);
    const auto end = bytes.substr(footer);
    EXPECT_TRUE(fails(head + b + end));
    EXPECT_TRUE(fails(head + a + a + b + end));
    EXPECT_TRUE(fails(head + b + a + end));
}

TEST(ArchiveBlocks, ReportsDestinationWriteFailure) {
    test_support::TemporaryDirectory temporary("dbackup-block-full");
    auto source = temporary.path() / "source";
    test_support::writeText(source, "contents");
    int in = open(source.c_str(), O_RDONLY);
    int full = open("/dev/full", O_WRONLY);
    ASSERT_GE(in, 0);
    ASSERT_GE(full, 0);
    std::string error;
    EXPECT_FALSE(dbackup::archive_blocks::encode(in, full, PackOptions{}, {}, error));
    EXPECT_NE(error.find("写入"), std::string::npos);
    close(in);
    close(full);
}

TEST(ArchiveBlocks, TarMetadataOffsetsReferToStoredFileData) {
    test_support::TemporaryDirectory temporary("dbackup-tar-metadata");
    dbackup::archive_format::Entry file;
    file.file.relativePath = "sample";
    file.file.type = 0;
    file.file.metadata.mode = 0100644;
    file.data = {'t', 'e', 's', 't'};
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(dbackup::archive_tar::encode({file}, bytes, error)) << error;
    auto path = temporary.path() / "archive.tar";
    test_support::writeText(path, std::string(bytes.begin(), bytes.end()));
    int fd = open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    std::vector<dbackup::archive_format::Entry> entries;
    std::vector<uint64_t> offsets;
    ASSERT_TRUE(dbackup::archive_tar::decodeMetadata(fd, entries, offsets, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(offsets.size(), 1u);
    EXPECT_TRUE(entries[0].data.empty());
    EXPECT_EQ(entries[0].file.metadata.size, 4u);
    char content[4];
    ASSERT_EQ(pread(fd, content, sizeof(content), offsets[0]), 4);
    EXPECT_EQ(std::string(content, sizeof(content)), "test");
    close(fd);
}

TEST(ArchiveBlocks, AuthenticatedTarIsVerifiedBeforeRestore) {
    test_support::TemporaryDirectory temporary("dbackup-wrapped-tar");
    const auto tarPath = temporary.path() / "packed";
    const auto wrappedPath = temporary.path() / "archive";
    const auto destination = temporary.path() / "restored";
    dbackup::archive_format::Entry file;
    file.file.relativePath = "sample";
    file.file.type = 0;
    file.file.metadata.mode = 0100644;
    file.file.metadata.uid = getuid();
    file.file.metadata.gid = getgid();
    file.data = {'t', 'e', 's', 't'};
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(dbackup::archive_tar::encode({file}, bytes, error)) << error;
    test_support::writeText(tarPath, std::string(bytes.begin(), bytes.end()));
    int in = open(tarPath.c_str(), O_RDONLY);
    int out = open(wrappedPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_GE(in, 0);
    ASSERT_GE(out, 0);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.password = "test-password";
    ASSERT_TRUE(dbackup::archive_blocks::encode(in, out, options, {}, error)) << error;
    close(in);
    close(out);
    EXPECT_FALSE(ArchiveService::extract(wrappedPath.string(), destination.string(), error, "wrong"));
    EXPECT_FALSE(std::filesystem::exists(destination / "sample"));
    ASSERT_TRUE(ArchiveService::extract(wrappedPath.string(), destination.string(), error,
                                        options.password)) << error;
    EXPECT_EQ(test_support::readText(destination / "sample"), "test");
}

TEST(ArchiveBlocks, ReadsPreviousChunkedIdentifier) {
    test_support::TemporaryDirectory temporary("dbackup-previous-blocks");
    const auto packed = temporary.path() / "packed";
    const auto archive = temporary.path() / "archive";
    dbackup::archive_format::Entry entry;
    entry.file.relativePath = "file";
    entry.file.metadata.mode = 0100644;
    entry.file.metadata.uid = getuid();
    entry.file.metadata.gid = getgid();
    entry.data = {'o', 'k'};
    std::vector<uint8_t> tar;
    std::string error;
    ASSERT_TRUE(dbackup::archive_tar::encode({entry}, tar, error)) << error;
    test_support::writeText(packed, std::string(tar.begin(), tar.end()));
    int in = open(packed.c_str(), O_RDONLY);
    int out = open(archive.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_GE(in, 0);
    ASSERT_GE(out, 0);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    ASSERT_TRUE(dbackup::archive_blocks::encode(in, out, options, {}, error)) << error;
    close(in);
    close(out);
    std::string bytes = test_support::readText(archive);
    ASSERT_GE(bytes.size(), 40u + 45u);
    bytes.replace(0, 8, "DBPK0002");
    std::vector<uint8_t> digestInput(bytes.begin(), bytes.begin() + 40);
    digestInput.insert(digestInput.end(), tar.begin(), tar.end());
    std::vector<uint8_t> digest;
    ASSERT_TRUE(dbackup::crypto::sha256(digestInput.data(), digestInput.size(), digest));
    std::copy(digest.begin(), digest.end(), bytes.end() - 32);
    test_support::writeText(archive, bytes);
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspectMetadata(archive.string(), entries, error)) << error;
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_TRUE(ArchiveService::extract(archive.string(), (temporary.path() / "output").string(),
                                        error)) << error;
    EXPECT_EQ(test_support::readText(temporary.path() / "output/file"), "ok");
}

TEST(PipelineAlgorithms, CompressionLimitAllowsTwoGibAndRejectsLarger) {
    const size_t limit = size_t(2) * 1024 * 1024 * 1024;
    std::vector<uint8_t> decoded;
    std::string error;
    EXPECT_FALSE(dbackup::compression::decode(Compression::None, {}, limit, decoded, error));
    EXPECT_NE(error.find("不匹配"), std::string::npos);
    EXPECT_FALSE(dbackup::compression::decode(Compression::None, {}, limit + 1, decoded, error));
    EXPECT_NE(error.find("超过限制"), std::string::npos);
}

TEST(PipelineAlgorithms, Md5Vectors) {
    EXPECT_EQ(dbackup::crypto::md5(nullptr, 0), "d41d8cd98f00b204e9800998ecf8427e");
    const std::string text = "abc";
    EXPECT_EQ(dbackup::crypto::md5(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
              "900150983cd24fb0d6963f7d28e17f72");
}
TEST(PipelineAlgorithms, KnownHuffmanAndLz77Vectors) {
    std::string error;
    std::vector<uint8_t> input{'a', 'a', 'a', 'b'}, encoded, decoded;
    ASSERT_TRUE(dbackup::compression::encode(Compression::Huffman, input, encoded, error));
    ASSERT_EQ(encoded.size(), 1025u);
    EXPECT_EQ(encoded[97 * 4], 3);
    EXPECT_EQ(encoded[98 * 4], 1);
    EXPECT_EQ(encoded.back(), 7);
    ASSERT_TRUE(
        dbackup::compression::decode(Compression::Huffman, encoded, input.size(), decoded, error));
    EXPECT_EQ(decoded, input);
    input = {'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b'};
    ASSERT_TRUE(dbackup::compression::encode(Compression::Lz77, input, encoded, error));
    EXPECT_EQ(encoded, (std::vector<uint8_t>{0, 'a', 0, 'b', 1, 2, 0, 3}));
    ASSERT_TRUE(
        dbackup::compression::decode(Compression::Lz77, encoded, input.size(), decoded, error));
    EXPECT_EQ(decoded, input);
    EXPECT_FALSE(dbackup::compression::decode(Compression::Lz77, {1, 0, 0, 0}, 3, decoded, error));
}
TEST(PipelineAlgorithms, CompressionAndCorruption) {
    std::mt19937 gen(42);
    std::vector<std::vector<uint8_t>> inputs{
        {}, {'A'}, std::vector<uint8_t>(10000, 'x'), std::vector<uint8_t>(9000)};
    for (auto& b : inputs.back())
        b = gen();
    for (auto method :
         {Compression::None, Compression::Huffman, Compression::Lz77, Compression::Deflate})
        for (const auto& input : inputs) {
            std::vector<uint8_t> encoded, decoded;
            std::string error;
            ASSERT_TRUE(dbackup::compression::encode(method, input, encoded, error)) << error;
            ASSERT_TRUE(dbackup::compression::decode(method, encoded, input.size(), decoded, error))
                << error;
            EXPECT_EQ(input, decoded);
            if (!encoded.empty()) {
                encoded.pop_back();
                EXPECT_FALSE(
                    dbackup::compression::decode(method, encoded, input.size(), decoded, error));
            }
        }
}
TEST(PipelineAlgorithms, DeflateCrossChecksZlib) {
    std::vector<uint8_t> input(4096, 'q'), encoded, decoded;
    std::string error;
    ASSERT_TRUE(dbackup::compression::encode(Compression::Deflate, input, encoded, error));
    decoded.resize(input.size());
    uLongf n = decoded.size();
    ASSERT_EQ(uncompress(decoded.data(), &n, encoded.data(), encoded.size()), Z_OK);
    EXPECT_EQ(decoded, input);
    uLongf size = compressBound(input.size());
    encoded.resize(size);
    ASSERT_EQ(compress2(encoded.data(), &size, input.data(), input.size(), 9), Z_OK);
    encoded.resize(size);
    ASSERT_TRUE(
        dbackup::compression::decode(Compression::Deflate, encoded, input.size(), decoded, error))
        << error;
    EXPECT_EQ(decoded, input);
    encoded.push_back(0);
    EXPECT_FALSE(
        dbackup::compression::decode(Compression::Deflate, encoded, input.size(), decoded, error));
}
TEST(PipelineAlgorithms, AllFormatCombinations) {
    using dbackup::archive_format::Entry;
    Entry e;
    e.file.relativePath = "long/path/file.txt";
    e.file.metadata.mode = 0100640;
    e.file.metadata.uid = 1000;
    e.file.metadata.gid = 1001;
    e.file.metadata.mtimeNs = 1700000000123456789LL;
    e.data.assign(5000, 'a');
    for (auto format : {PackFormat::Custom, PackFormat::Tar})
        for (auto comp :
             {Compression::None, Compression::Huffman, Compression::Lz77, Compression::Deflate})
            for (bool encrypted : {false, true}) {
                PackOptions options;
                options.format = format;
                options.compression = comp;
                options.password = encrypted ? "test-password" : "";
                std::string error;
                std::vector<uint8_t> encoded;
                std::vector<Entry> decoded;
                ASSERT_TRUE(dbackup::archive_format::encodeCurrent({e}, options, encoded, error))
                    << error;
                if (encrypted)
                    EXPECT_FALSE(
                        dbackup::archive_format::decodeCurrent(encoded, "wrong", decoded, error));
                ASSERT_TRUE(dbackup::archive_format::decodeCurrent(encoded, options.password,
                                                                   decoded, error))
                    << error;
                ASSERT_EQ(decoded.size(), 1u);
                EXPECT_EQ(decoded[0].data, e.data);
                EXPECT_EQ(decoded[0].file.relativePath, e.file.relativePath);
                EXPECT_EQ(decoded[0].file.metadata.mtimeNs, e.file.metadata.mtimeNs);
                if (format != PackFormat::Tar || comp != Compression::None || encrypted) {
                    encoded[encoded.size() / 2] ^= 1;
                    EXPECT_FALSE(dbackup::archive_format::decodeCurrent(encoded, options.password,
                                                                        decoded, error));
                }
            }
}

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

// 验证增量扫描、清单、备份和版本链
#include "core/incremental/incremental_diff.h"
#include "core/incremental/incremental_scan.h"
#include "core/incremental/incremental_manifest.h"
#include "core/incremental/incremental_archive.h"
#include "core/incremental/incremental_chain.h"
#include "test_support.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace dbackup::incremental;

static std::filesystem::path versionPath(const std::filesystem::path& chain, uint64_t sequence) {
    for (const auto& item : std::filesystem::directory_iterator(chain)) {
        if (!item.is_directory()) continue;
        std::ifstream file(item.path() / "manifest", std::ios::binary);
        if (!file) continue;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        Manifest manifest;
        std::string error;
        if ((decodeManifest(bytes, manifest, error) ||
             decodeProtectedManifest(bytes, "test password", manifest, error) ||
             decodeProtectedManifest(bytes, "password", manifest, error)) &&
            manifest.sequence == sequence) return item.path();
    }
    return {};
}

TEST(IncrementalDiff, FindsAddedChangedAndDeletedPaths) {
    FileState old{"a"}, unchanged{"b"}, removed{"c"};
    FileState modified = old;
    modified.contentDigest = "new-content";
    FileState added{"d"};
    Changes result;
    std::string error;
    ASSERT_TRUE(compare({old, unchanged, removed}, {modified, unchanged, added}, result, error));
    EXPECT_EQ(result.changed, (std::vector<std::string>{"a", "d"}));
    EXPECT_EQ(result.deleted, (std::vector<std::string>{"c"}));
}

TEST(IncrementalDiff, DetectsTypeAndLinkChanges) {
    FileState previous{"x"};
    FileState current = previous;
    current.type = 2;
    current.linkTarget = "target";
    Changes result;
    std::string error;
    ASSERT_TRUE(compare({previous}, {current}, result, error));
    EXPECT_EQ(result.changed, (std::vector<std::string>{"x"}));
}

TEST(IncrementalDiff, RejectsUnsafeAndDuplicatePathsWithoutReplacingOutput) {
    Changes result{{"sentinel"}, {}};
    std::string error;
    EXPECT_FALSE(compare({}, {FileState{"../outside"}}, result, error));
    EXPECT_EQ(result.changed, (std::vector<std::string>{"sentinel"}));
    EXPECT_FALSE(compare({}, {FileState{"a"}, FileState{"a"}}, result, error));
    EXPECT_FALSE(error.empty());
}

TEST(IncrementalScan, ComparesActualFilesAndPreservesOutputOnFailure) {
    test_support::TemporaryDirectory temporary("dbackup-incremental");
    const auto source = std::filesystem::path(temporary.string()) / "source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "old") << "old";
    std::vector<FileState> before, after;
    std::string error;
    ASSERT_TRUE(scan({source.string()}, {}, {}, before, error)) << error;
    std::filesystem::remove(source / "old");
    std::ofstream(source / "new") << "new";
    ASSERT_TRUE(scan({source.string()}, {}, {}, after, error)) << error;
    Changes changes;
    ASSERT_TRUE(compare(before, after, changes, error)) << error;
    EXPECT_NE(std::find(changes.changed.begin(), changes.changed.end(), "source/new"),
              changes.changed.end());
    EXPECT_NE(std::find(changes.deleted.begin(), changes.deleted.end(), "source/old"),
              changes.deleted.end());
    const auto size = after.size();
    EXPECT_FALSE(scan({(source / "missing").string()}, {}, {}, after, error));
    EXPECT_EQ(after.size(), size);
}

TEST(IncrementalScan, RejectsPartialScan) {
    if (geteuid() == 0)
        GTEST_SKIP() << "需要非 root 用户";
    test_support::TemporaryDirectory temporary("dbackup-incremental-private");
    const auto source = std::filesystem::path(temporary.string()) / "source";
    std::filesystem::create_directories(source / "private");
    std::ofstream(source / "private" / "file") << "data";
    ASSERT_EQ(chmod((source / "private").c_str(), 0), 0);
    std::vector<FileState> states{{"sentinel"}};
    std::string error;
    const bool ok = scan({source.string()}, {}, {}, states, error);
    chmod((source / "private").c_str(), 0700);
    EXPECT_FALSE(ok);
    EXPECT_EQ(states.size(), 1u);
    EXPECT_EQ(states.front().path, "sentinel");
    EXPECT_FALSE(error.empty());
}

TEST(IncrementalScan, DetectsSameSizeAndTimestampContentChange) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-hash");
    const auto source = std::filesystem::path(temporary.string()) / "source";
    std::filesystem::create_directory(source);
    const auto file = source / "file";
    std::ofstream(file) << "abc";
    std::vector<FileState> before, after;
    std::string error;
    ASSERT_TRUE(scan({source.string()}, {}, {}, before, error)) << error;
    auto it = std::find_if(before.begin(), before.end(), [](const auto& f) {
        return f.path == "source/file";
    });
    ASSERT_NE(it, before.end());
    EXPECT_EQ(it->contentDigest,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    struct stat original {};
    ASSERT_EQ(stat(file.c_str(), &original), 0);
    std::ofstream(file) << "xyz";
    const timespec times[2] = {{0, UTIME_OMIT}, original.st_mtim};
    ASSERT_EQ(utimensat(AT_FDCWD, file.c_str(), times, 0), 0);
    ASSERT_TRUE(scan({source.string()}, {}, {}, after, error)) << error;
    Changes changes;
    ASSERT_TRUE(compare(before, after, changes, error)) << error;
    EXPECT_NE(std::find(changes.changed.begin(), changes.changed.end(), "source/file"),
              changes.changed.end());
}

TEST(IncrementalManifest, RoundTripsAndRejectsCorruption) {
    Manifest original;
    original.sequence = 2;
    original.previous = 1;
    FileState file{"directory/file"};
    file.mtimeNs = -123;
    file.size = 42;
    file.contentDigest = "digest";
    original.files.push_back(file);
    original.deleted = {"old-file"};
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(encodeManifest(original, bytes, error)) << error;
    Manifest decoded;
    ASSERT_TRUE(decodeManifest(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.sequence, 2u);
    EXPECT_EQ(decoded.previous, 1u);
    ASSERT_EQ(decoded.files.size(), 1u);
    EXPECT_EQ(decoded.files[0].mtimeNs, -123);
    EXPECT_EQ(decoded.files[0].contentDigest, "digest");
    EXPECT_EQ(decoded.deleted, original.deleted);
    bytes[10] ^= 1;
    EXPECT_FALSE(decodeManifest(bytes, decoded, error));
    EXPECT_EQ(decoded.sequence, 2u);
}

TEST(IncrementalManifest, RejectsInvalidChainsAndPaths) {
    Manifest m;
    m.sequence = 1;
    m.previous = 1;
    std::vector<uint8_t> bytes{42};
    std::string error;
    EXPECT_FALSE(encodeManifest(m, bytes, error));
    EXPECT_EQ(bytes, (std::vector<uint8_t>{42}));
    m.full = true;
    m.previous = 0;
    m.files = {FileState{"../escape"}};
    EXPECT_FALSE(encodeManifest(m, bytes, error));
    m.files.clear();
    m.deleted = {"old"};
    EXPECT_FALSE(encodeManifest(m, bytes, error));
    m.full = false;
    m.sequence = 2;
    m.previous = 1;
    m.files = {FileState{"old"}};
    EXPECT_FALSE(encodeManifest(m, bytes, error));
}

TEST(IncrementalManifest, EncryptsAuthenticatesAndRejectsDowngrades) {
    Manifest original;
    original.sequence = 1;
    original.full = true;
    original.files = {FileState{"private-filename"}};
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(encodeProtectedManifest(original, "secret", bytes, error)) << error;
    const std::string encoded(bytes.begin(), bytes.end());
    EXPECT_EQ(encoded.find("private-filename"), std::string::npos);
    Manifest decoded;
    ASSERT_TRUE(decodeProtectedManifest(bytes, "secret", decoded, error)) << error;
    EXPECT_EQ(decoded.files[0].path, "private-filename");
    EXPECT_FALSE(decodeProtectedManifest(bytes, "wrong", decoded, error));
    EXPECT_EQ(decoded.files[0].path, "private-filename");
    bytes.back() ^= 1;
    EXPECT_FALSE(decodeProtectedManifest(bytes, "secret", decoded, error));
    EXPECT_FALSE(encodeProtectedManifest(original, "", bytes, error));
    std::vector<uint8_t> plain;
    ASSERT_TRUE(encodeManifest(original, plain, error));
    EXPECT_FALSE(decodeProtectedManifest(plain, "secret", decoded, error));
}

TEST(IncrementalChain, ValidatesDependenciesAndDeletionRecords) {
    Manifest full;
    full.sequence = 1;
    full.full = true;
    full.files = {FileState{"a"}, FileState{"b"}};
    Manifest delta;
    delta.sequence = 2;
    delta.previous = 1;
    delta.files = {FileState{"b"}, FileState{"c"}};
    delta.deleted = {"a"};
    std::string error;
    EXPECT_TRUE(validateChain({full, delta}, error)) << error;
    delta.previous = 3;
    EXPECT_FALSE(validateChain({full, delta}, error));
    delta.previous = 1;
    delta.deleted.clear();
    EXPECT_FALSE(validateChain({full, delta}, error));
    EXPECT_FALSE(validateChain({delta}, error));
}

TEST(IncrementalChain, RetentionKeepsTheRequiredFullBackup) {
    Manifest a;
    a.sequence = 1;
    a.full = true;
    Manifest b;
    b.sequence = 2;
    b.previous = 1;
    Manifest c;
    c.sequence = 3;
    c.full = true;
    Manifest d;
    d.sequence = 4;
    d.previous = 3;
    size_t count = 999;
    std::string error;
    ASSERT_TRUE(removablePrefix({a, b, c, d}, 1, count, error)) << error;
    EXPECT_EQ(count, 2u);
    ASSERT_TRUE(removablePrefix({a, b, c, d}, 3, count, error)) << error;
    EXPECT_EQ(count, 0u);
    EXPECT_FALSE(removablePrefix({a, b, c, d}, 0, count, error));
    EXPECT_EQ(count, 0u);
}

TEST(IncrementalArchive, WritesSelectedHardLinkAsIndependentFile) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-tar");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "a") << "contents";
    std::filesystem::create_hard_link(source / "a", source / "b");
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    ASSERT_TRUE(dbackup::archive_scan::collect({source.string()}, {}, {}, entries, nullptr, {},
                                              error, true)) << error;
    auto hard = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.type == 3; });
    ASSERT_NE(hard, entries.end());
    const auto archive = (root / "delta.tar").string();
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    ASSERT_TRUE(writeSelectedArchive(entries, {hard->relativePath}, archive, options, {}, error))
        << error;
    std::vector<ArchiveEntry> saved;
    ASSERT_TRUE(ArchiveService::inspect(archive, saved, error)) << error;
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].relativePath, hard->relativePath);
    EXPECT_EQ(saved[0].type, 0);
    EXPECT_EQ(saved[0].data, "contents");
}

TEST(IncrementalArchive, RejectsUnknownPathsAndDoesNotOverwriteOnFailure) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-target");
    const auto root = std::filesystem::path(temporary.string());
    const auto archive = root / "delta.tar";
    std::ofstream(archive) << "existing";
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    OperationContext context;
    context.noReplace = true;
    std::string error;
    EXPECT_FALSE(writeSelectedArchive({}, {"missing"}, archive.string(), options, context, error));
    EXPECT_FALSE(writeSelectedArchive({}, {}, archive.string(), options, context, error));
    std::ifstream input(archive);
    std::string content;
    input >> content;
    EXPECT_EQ(content, "existing");
}

TEST(IncrementalArchive, SupportsCompressedEncryptedTarAndCancellation) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-wrapped");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "item") << "private contents";
    std::vector<dbackup::archive_scan::Entry> entries;
    std::string error;
    ASSERT_TRUE(dbackup::archive_scan::collect({source.string()}, {}, {}, entries, nullptr, {},
                                              error, true)) << error;
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::Deflate;
    options.password = "test password";
    const auto archive = (root / "delta.abk").string();
    ASSERT_TRUE(writeSelectedArchive(entries, {"source/item"}, archive, options, {}, error))
        << error;
    std::vector<ArchiveEntry> decoded;
    ASSERT_TRUE(ArchiveService::inspect(archive, decoded, error, options.password)) << error;
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded.front().data, "private contents");
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(writeSelectedArchive(entries, {"source/item"}, archive, options, context, error));
    ASSERT_TRUE(ArchiveService::inspect(archive, decoded, error, options.password)) << error;
    EXPECT_EQ(decoded.front().data, "private contents");
}

TEST(IncrementalArchive, ChecksArchiveEntriesAgainstVersionState) {
    Manifest before;
    before.sequence = 1;
    before.full = true;
    before.files = {FileState{"unchanged"}, FileState{"deleted"}};
    Manifest after;
    after.sequence = 2;
    after.previous = 1;
    FileState added{"added"};
    added.size = 3;
    after.files = {FileState{"unchanged"}, added};
    after.deleted = {"deleted"};
    ArchiveEntry entry{};
    entry.relativePath = "added";
    entry.type = 0;
    entry.originalSize = 3;
    std::string error;
    EXPECT_TRUE(validateArchiveEntries(after, &before, {entry}, error)) << error;
    EXPECT_FALSE(validateArchiveEntries(after, &before, {}, error));
    EXPECT_FALSE(validateArchiveEntries(after, &before, {entry, entry}, error));
    entry.originalSize = 4;
    EXPECT_FALSE(validateArchiveEntries(after, &before, {entry}, error));
    after.deleted.clear();
    EXPECT_FALSE(validateArchiveEntries(after, &before, {entry}, error));
}

TEST(IncrementalArchive, BuildsAndChecksDeltaFromRealScans) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-flow");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "old") << "old";
    std::string error;
    Manifest before;
    before.sequence = 1;
    before.full = true;
    ASSERT_TRUE(scan({source.string()}, {}, {}, before.files, error)) << error;
    std::filesystem::remove(source / "old");
    std::ofstream(source / "new") << "new content";
    Manifest after;
    after.sequence = 2;
    after.previous = 1;
    ASSERT_TRUE(scan({source.string()}, {}, {}, after.files, error)) << error;
    Changes changes;
    ASSERT_TRUE(compare(before.files, after.files, changes, error)) << error;
    after.deleted = changes.deleted;
    std::vector<dbackup::archive_scan::Entry> scanned;
    ASSERT_TRUE(dbackup::archive_scan::collect({source.string()}, {}, {}, scanned, nullptr, {},
                                              error, true)) << error;
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    const auto archive = (root / "delta.tar").string();
    ASSERT_TRUE(writeSelectedArchive(scanned, changes.changed, archive, options, {}, error)) << error;
    std::vector<ArchiveEntry> entries;
    ASSERT_TRUE(ArchiveService::inspectMetadata(archive, entries, error)) << error;
    EXPECT_TRUE(validateArchiveEntries(after, &before, entries, error)) << error;
    ASSERT_TRUE(verifyArchive(archive, "", after, &before, {}, error)) << error;
    std::fstream contents(archive, std::ios::in | std::ios::out | std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(contents)), std::istreambuf_iterator<char>());
    const auto pos = raw.find("new content");
    ASSERT_NE(pos, std::string::npos);
    contents.clear();
    contents.seekp(static_cast<std::streamoff>(pos));
    contents.put('X');
    contents.close();
    EXPECT_FALSE(verifyArchive(archive, "", after, &before, {}, error));
}

TEST(IncrementalArchive, VerifiesEncryptedArchiveAndRejectsWrongPassword) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-verify");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "a") << "same content";
    std::filesystem::create_hard_link(source / "a", source / "b");
    Manifest manifest;
    manifest.sequence = 1;
    manifest.full = true;
    std::string error;
    ASSERT_TRUE(scan({source.string()}, {}, {}, manifest.files, error)) << error;
    std::vector<dbackup::archive_scan::Entry> entries;
    ASSERT_TRUE(dbackup::archive_scan::collect({source.string()}, {}, {}, entries, nullptr, {},
                                              error, true)) << error;
    std::vector<std::string> selected;
    for (const auto& f : manifest.files)
        selected.push_back(f.path);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::Deflate;
    options.password = "correct password";
    const auto archive = (root / "full.abk").string();
    ASSERT_TRUE(writeSelectedArchive(entries, selected, archive, options, {}, error)) << error;
    EXPECT_TRUE(verifyArchive(archive, options.password, manifest, nullptr, {}, error)) << error;
    EXPECT_FALSE(verifyArchive(archive, "wrong password", manifest, nullptr, {}, error));
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(verifyArchive(archive, options.password, manifest, nullptr, context, error));
}

TEST(IncrementalChain, CreatesListsAndRestoresVersionsWithoutOverwriting) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-e2e");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto chain = root / "chain";
    std::filesystem::create_directory(source);
    std::ofstream(source / "removed") << "original";
    std::ofstream(source / "kept") << "one";
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::Deflate;
    options.password = "test password";
    CreatedVersion first, second;
    std::string error;
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, first, error)) << error;
    EXPECT_TRUE(first.full);
    std::filesystem::remove(source / "removed");
    std::ofstream(source / "kept") << "two";
    std::ofstream(source / "added") << "new";
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, second, error)) << error;
    EXPECT_FALSE(second.full);
    std::vector<VersionInfo> versions;
    ASSERT_TRUE(listVersions(chain.string(), options.password, {}, versions, error)) << error;
    ASSERT_EQ(versions.size(), 2u);
    EXPECT_TRUE(versions[1].changesAvailable);
    EXPECT_EQ(versions[1].added, std::vector<std::string>({"source/added"}));
    EXPECT_EQ(versions[1].modified, std::vector<std::string>({"source/kept"}));
    EXPECT_EQ(versions[1].deleted, std::vector<std::string>({"source/removed"}));
    EXPECT_EQ(second.added, versions[1].added);
    EXPECT_EQ(second.modified, versions[1].modified);
    EXPECT_EQ(second.deleted, versions[1].deleted);
    EXPECT_FALSE(listVersions(chain.string(), "wrong", {}, versions, error));
    const auto firstOut = root / "restored-first";
    const auto secondOut = root / "restored-second";
    ASSERT_TRUE(restoreVersion(chain.string(), first.sequence, options.password,
                               firstOut.string(), {}, error)) << error;
    ASSERT_TRUE(restoreVersion(chain.string(), second.sequence, options.password,
                               secondOut.string(), {}, error)) << error;
    EXPECT_TRUE(std::filesystem::exists(firstOut / "source" / "removed"));
    EXPECT_FALSE(std::filesystem::exists(secondOut / "source" / "removed"));
    EXPECT_TRUE(std::filesystem::exists(secondOut / "source" / "added"));
    std::ifstream restored(secondOut / "source" / "kept");
    std::string content;
    restored >> content;
    EXPECT_EQ(content, "two");
    EXPECT_FALSE(restoreVersion(chain.string(), second.sequence, options.password,
                                secondOut.string(), {}, error));
    EXPECT_TRUE(std::filesystem::exists(secondOut / "source" / "added"));
}

TEST(IncrementalChain, RestoresCustomAndTarVersionsInSameChain) {
    for (bool encrypted : {false, true}) {
        test_support::TemporaryDirectory temporary("dbackup-incremental-custom");
        const auto root = std::filesystem::path(temporary.string());
        const auto source = root / "source";
        const auto chain = root / "chain";
        std::filesystem::create_directory(source);
        std::ofstream(source / "removed") << "original";
        std::ofstream(source / "kept") << "one";
        PackOptions options;
        options.format = PackFormat::Custom;
        options.compression = encrypted ? Compression::Deflate : Compression::None;
        options.password = encrypted ? "test password" : "";
        CreatedVersion first, second, third;
        std::string error;
        ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, first, error)) << error;
        auto firstDir = versionPath(chain, 1);
        ASSERT_FALSE(firstDir.empty());
        EXPECT_NE(firstDir.filename().string().find("-full"), std::string::npos);
        std::filesystem::rename(firstDir, chain / "v00000000000000000001");
        const auto firstArchive = chain / "v00000000000000000001" / "archive.abk";
        ASSERT_TRUE(std::filesystem::exists(firstArchive));
        // Existing chains used an extensionless name; preserve their readability
        std::filesystem::rename(firstArchive, firstArchive.parent_path() / "archive");
        std::filesystem::remove(source / "removed");
        std::ofstream(source / "kept") << "two";
        ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, second, error)) << error;
        EXPECT_FALSE(second.full);
        EXPECT_TRUE(std::filesystem::exists(versionPath(chain, 2) / "archive.abk"));
        std::filesystem::rename(versionPath(chain, 2), chain / "20261111-201830-incremental-2");
        options.format = PackFormat::Tar;
        std::ofstream(source / "added") << "three";
        ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, third, error)) << error;
        EXPECT_FALSE(third.full);
        EXPECT_TRUE(std::filesystem::exists(versionPath(chain, 3) /
                                            (encrypted ? "archive.abk" : "archive.tar")));
        std::vector<VersionInfo> versions;
        ASSERT_TRUE(listVersions(chain.string(), options.password, {}, versions, error)) << error;
        ASSERT_EQ(versions.size(), 3u);
        const auto firstOut = root / "first";
        const auto lastOut = root / "last";
        ASSERT_TRUE(restoreVersion(chain.string(), first.sequence, options.password, firstOut.string(), {}, error)) << error;
        ASSERT_TRUE(restoreVersion(chain.string(), third.sequence, options.password, lastOut.string(), {}, error)) << error;
        EXPECT_TRUE(std::filesystem::exists(firstOut / "source" / "removed"));
        EXPECT_FALSE(std::filesystem::exists(lastOut / "source" / "removed"));
        EXPECT_TRUE(std::filesystem::exists(lastOut / "source" / "added"));
        std::ifstream restored(lastOut / "source" / "kept");
        std::string content;
        restored >> content;
        EXPECT_EQ(content, "two");
        if (encrypted)
            EXPECT_FALSE(restoreVersion(chain.string(), third.sequence, "wrong", (root / "wrong").string(), {}, error));
    }
}

TEST(IncrementalChain, PrunesOnlyWholeObsoleteChains) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-prune");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto chain = root / "chain";
    std::filesystem::create_directory(source);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion created;
    std::string error;
    for (int i = 1; i <= 8; ++i) {
        std::ofstream(source / "item") << "version-" << i;
        ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, created, error))
            << error;
    }
    EXPECT_TRUE(created.full);
    size_t removed = 0;
    ASSERT_TRUE(pruneVersions(chain.string(), "", 1, {}, removed, error)) << error;
    EXPECT_EQ(removed, 7u);
    std::vector<VersionInfo> versions;
    ASSERT_TRUE(listVersions(chain.string(), "", {}, versions, error)) << error;
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].sequence, 8u);
    EXPECT_TRUE(versions[0].full);
    ASSERT_TRUE(restoreVersion(chain.string(), 8, "", (root / "restored").string(), {}, error))
        << error;
}

TEST(IncrementalChain, StrictRetentionRebasesOldestKeptVersion) {
    for (bool encrypted : {false, true}) {
        test_support::TemporaryDirectory temporary("dbackup-strict-retention");
        const auto root = std::filesystem::path(temporary.string());
        const auto source = root / "source";
        const auto chain = root / "chain";
        std::filesystem::create_directory(source);
        PackOptions options;
        options.format = encrypted ? PackFormat::Custom : PackFormat::Tar;
        options.compression = encrypted ? Compression::Deflate : Compression::None;
        options.password = encrypted ? "password" : "";
        CreatedVersion created;
        std::string error;
        for (int i = 1; i <= 5; ++i) {
            std::ofstream(source / "item") << "version-" << i;
            ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, created, error)) << error;
        }
        size_t removed = 0;
        std::atomic_bool cancelled{true};
        OperationContext cancelledContext;
        cancelledContext.cancelled = &cancelled;
        EXPECT_FALSE(pruneVersions(chain.string(), options.password, 3, cancelledContext,
                                   removed, error, &options));
        std::vector<VersionInfo> before;
        ASSERT_TRUE(listVersions(chain.string(), options.password, {}, before, error)) << error;
        EXPECT_EQ(before.size(), 5u);
        ASSERT_TRUE(pruneVersions(chain.string(), options.password, 3, {}, removed, error, &options)) << error;
        EXPECT_EQ(removed, 2u);
        std::vector<VersionInfo> versions;
        ASSERT_TRUE(listVersions(chain.string(), options.password, {}, versions, error)) << error;
        ASSERT_EQ(versions.size(), 3u);
        EXPECT_EQ(versions[0].sequence, 3u);
        EXPECT_TRUE(versions[0].full);
        EXPECT_FALSE(versions[0].changesAvailable);
        std::vector<VersionInfo> publicVersions;
        ASSERT_TRUE(listPublicVersions(chain.string(), {}, publicVersions, error)) << error;
        ASSERT_EQ(publicVersions.size(), 3u);
        EXPECT_EQ(publicVersions[0].sequence, 3u);
        EXPECT_TRUE(publicVersions[0].full);
        EXPECT_TRUE(publicVersions[0].changesAvailable);
        EXPECT_EQ(publicVersions[0].modified, std::vector<std::string>({"source/item"}));
        EXPECT_NE(versionPath(chain, 3).filename().string().find("-full"), std::string::npos);
        for (size_t i = 0; i < versions.size(); ++i) {
            auto output = root / ("restore-" + std::to_string(i));
            ASSERT_TRUE(restoreVersion(chain.string(), versions[i].sequence, options.password,
                                       output.string(), {}, error)) << error;
            std::ifstream file(output / "source" / "item");
            std::string content; file >> content;
            EXPECT_EQ(content, "version-" + std::to_string(i + 3));
        }
        ASSERT_TRUE(pruneVersions(chain.string(), options.password, 1, {}, removed, error, &options)) << error;
        ASSERT_TRUE(listVersions(chain.string(), options.password, {}, versions, error)) << error;
        ASSERT_EQ(versions.size(), 1u);
        EXPECT_EQ(versions[0].sequence, 5u);
        EXPECT_TRUE(versions[0].full);
    }
}

TEST(IncrementalChain, StartsNewFullVersionWhenHardLinkGroupChanges) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-hardlinks");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto chain = root / "chain";
    std::filesystem::create_directory(source);
    std::ofstream(source / "a") << "contents";
    std::filesystem::create_hard_link(source / "a", source / "b");
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion first, second;
    std::string error;
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, first, error)) << error;
    std::filesystem::create_hard_link(source / "a", source / "c");
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, second, error)) << error;
    EXPECT_TRUE(second.full);
    const auto restored = root / "restored";
    ASSERT_TRUE(restoreVersion(chain.string(), second.sequence, "", restored.string(), {}, error))
        << error;
    struct stat a {}, b {}, c {};
    ASSERT_EQ(stat((restored / "source/a").c_str(), &a), 0);
    ASSERT_EQ(stat((restored / "source/b").c_str(), &b), 0);
    ASSERT_EQ(stat((restored / "source/c").c_str(), &c), 0);
    EXPECT_EQ(a.st_ino, b.st_ino);
    EXPECT_EQ(a.st_ino, c.st_ino);
}

TEST(IncrementalChain, RestoresFileDirectoryAndSymlinkTransitions) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-types");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto chain = root / "chain";
    std::filesystem::create_directories(source / "node");
    std::ofstream(source / "node" / "child") << "old";
    std::ofstream(source / "target") << "target";
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion version;
    std::string error;
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, version, error)) << error;
    std::filesystem::remove(source / "node" / "child");
    std::filesystem::remove(source / "node");
    std::ofstream(source / "node") << "file";
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, version, error)) << error;
    std::filesystem::remove(source / "node");
    std::filesystem::create_symlink("target", source / "node");
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, version, error)) << error;
    const auto restored = root / "restored";
    ASSERT_TRUE(restoreVersion(chain.string(), version.sequence, "", restored.string(), {}, error))
        << error;
    EXPECT_TRUE(std::filesystem::is_symlink(restored / "source" / "node"));
    EXPECT_EQ(std::filesystem::read_symlink(restored / "source" / "node"), "target");
    EXPECT_FALSE(std::filesystem::exists(restored / "source" / "node" / "child"));
}

TEST(IncrementalChain, RejectsCorruptedArchiveAndCancelledVersion) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-corrupt");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto chain = root / "chain";
    std::filesystem::create_directory(source);
    std::ofstream(source / "file") << "original-contents";
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion version;
    std::string error;
    ASSERT_TRUE(createVersion(chain.string(), {source.string()}, options, {}, version, error)) << error;
    std::atomic_bool cancelled{true};
    OperationContext context;
    context.cancelled = &cancelled;
    EXPECT_FALSE(createVersion(chain.string(), {source.string()}, options, context, version, error));
    std::vector<VersionInfo> versions;
    ASSERT_TRUE(listVersions(chain.string(), "", {}, versions, error)) << error;
    ASSERT_EQ(versions.size(), 1u);
    const auto archive = versionPath(chain, 1) / "archive.tar";
    std::fstream file(archive, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto offset = bytes.find("original-contents");
    ASSERT_NE(offset, std::string::npos);
    file.clear();
    file.seekp(static_cast<std::streamoff>(offset));
    file.put('X');
    file.close();
    EXPECT_FALSE(listVersions(chain.string(), "", {}, versions, error));
    EXPECT_FALSE(restoreVersion(chain.string(), 1, "", (root / "restored").string(), {}, error));
    EXPECT_FALSE(std::filesystem::exists(root / "restored"));
}

TEST(IncrementalChain, RestoresFifoAndSocketWithNotice) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-special");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    ASSERT_EQ(mkfifo((source / "fifo").c_str(), 0600), 0);
    const auto socketPath = (source / "socket").string();
    ASSERT_LT(socketPath.size(), sizeof(sockaddr_un::sun_path));
    const int socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(socketFd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
    const int bound = bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close(socketFd);
    ASSERT_EQ(bound, 0);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion created;
    std::string error;
    ASSERT_TRUE(createVersion((root / "chain").string(), {source.string()}, options, {}, created, error))
        << error;
    ASSERT_TRUE(restoreVersion((root / "chain").string(), created.sequence, "",
                               (root / "restored").string(), {}, error)) << error;
    EXPECT_FALSE(error.empty());
    struct stat fifo {}, socketInfo {};
    ASSERT_EQ(lstat((root / "restored/source/fifo").c_str(), &fifo), 0);
    ASSERT_EQ(lstat((root / "restored/source/socket").c_str(), &socketInfo), 0);
    EXPECT_TRUE(S_ISFIFO(fifo.st_mode));
    EXPECT_TRUE(S_ISSOCK(socketInfo.st_mode));
}

TEST(IncrementalChain, RestoresChangesInsideReadOnlyDirectory) {
    test_support::TemporaryDirectory temporary("dbackup-incremental-readonly");
    const auto root = std::filesystem::path(temporary.string());
    const auto source = root / "source";
    const auto folder = source / "readonly";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "old") << "old";
    ASSERT_EQ(chmod(folder.c_str(), 0555), 0);
    PackOptions options;
    options.format = PackFormat::Tar;
    options.compression = Compression::None;
    CreatedVersion version;
    std::string error;
    ASSERT_TRUE(createVersion((root / "chain").string(), {source.string()}, options, {}, version, error))
        << error;
    ASSERT_EQ(chmod(folder.c_str(), 0755), 0);
    std::filesystem::remove(folder / "old");
    std::ofstream(folder / "new") << "new";
    ASSERT_EQ(chmod(folder.c_str(), 0555), 0);
    ASSERT_TRUE(createVersion((root / "chain").string(), {source.string()}, options, {}, version, error))
        << error;
    const auto target = root / "restored";
    ASSERT_TRUE(restoreVersion((root / "chain").string(), version.sequence, "", target.string(), {}, error))
        << error;
    EXPECT_FALSE(std::filesystem::exists(target / "source/readonly/old"));
    EXPECT_TRUE(std::filesystem::exists(target / "source/readonly/new"));
    struct stat info {};
    ASSERT_EQ(stat((target / "source/readonly").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0555);
    chmod(folder.c_str(), 0755);
    chmod((target / "source/readonly").c_str(), 0755);
}

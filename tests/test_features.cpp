#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <string>

#include "automation/cron.h"
#include "automation/directory_watcher.h"
#include "core/repository.h"

namespace {
std::string tempPath(const char *name) {
    struct TempRoot {
        std::filesystem::path path;
        TempRoot() { std::string value = (std::filesystem::temp_directory_path() / "dbackup-features-XXXXXX").string(); char *result = mkdtemp(value.data()); if (result) path = result; }
        ~TempRoot() { if (!path.empty()) std::filesystem::remove_all(path); }
    };
    static TempRoot root;
    return (root.path / name).string();
}
void writeText(const std::string &path, const std::string &value) { std::ofstream out(path, std::ios::binary); out << value; }
std::string readText(const std::string &path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
}

TEST(CronExpressionTest, ParsesRangesStepsAndFindsNextRun) {
    CronExpression cron; std::string error;
    ASSERT_TRUE(cron.parse("*/15 9-17 * * 1-5", error)) << error;
    std::tm value = {}; value.tm_min=30; value.tm_hour=10; value.tm_mday=15; value.tm_mon=5; value.tm_wday=1;
    EXPECT_TRUE(cron.matches(value));
    value.tm_min=31; EXPECT_FALSE(cron.matches(value));
    EXPECT_FALSE(cron.parse("61 * * * *", error));
    std::tm start={};start.tm_year=126;start.tm_mon=0;start.tm_mday=5;start.tm_hour=10;start.tm_min=31;start.tm_isdst=-1;
    std::time_t next=cron.next(std::mktime(&start));ASSERT_NE(next,static_cast<std::time_t>(-1));std::tm result={};localtime_r(&next,&result);EXPECT_EQ(result.tm_min,45);
}

TEST(LocalRepositoryTest, DeduplicatesAndRestoresEncryptedChunks) {
    const std::string root=tempPath("repo"), source=tempPath("source"), restore=tempPath("restore");
    std::filesystem::create_directory(source); writeText(source+"/a.bin",std::string(8192,'A')); writeText(source+"/b.bin",std::string(8192,'A'));
    LocalRepository repository(root); std::string error; ASSERT_TRUE(repository.initialize("secret",error))<<error;
    SnapshotOptions options; options.sources={source}; options.password="secret"; options.chunkSize=4096;
    SnapshotInfo first; ASSERT_TRUE(repository.createSnapshot(options,first,error))<<error; EXPECT_EQ(first.fileCount,2u);
    SnapshotInfo second; ASSERT_TRUE(repository.createSnapshot(options,second,error))<<error; EXPECT_EQ(second.storedBytes,0u);
    ASSERT_TRUE(repository.restoreSnapshot(second.id,restore,"secret",error))<<error;
    const std::string folder=restore+"/"+source.substr(source.find_last_of("//")+1);
    EXPECT_EQ(readText(folder+"/a.bin"),std::string(8192,'A'));
    EXPECT_FALSE(repository.restoreSnapshot(second.id,restore,"wrong",error));
    repository.prune(1,error);
    std::vector<SnapshotInfo> snapshots; ASSERT_TRUE(repository.listSnapshots(snapshots,error)); EXPECT_EQ(snapshots.size(),1u);
    
}

TEST(LocalRepositoryTest, SingleFileSourceDoesNotIncludeItsSiblingsAndHonorsCancellation) {
    const std::string root=tempPath("single_repo"), source=tempPath("single_source"), restore=tempPath("single_restore");
    std::filesystem::create_directory(source);writeText(source+"/selected.txt","selected");writeText(source+"/sibling.txt","must not be copied");
    LocalRepository repository(root);std::string error;SnapshotOptions options;options.sources={source+"/selected.txt"};options.password="secret";SnapshotInfo info;
    ASSERT_TRUE(repository.createSnapshot(options,info,error))<<error;EXPECT_EQ(info.fileCount,1u);ASSERT_TRUE(repository.restoreSnapshot(info.id,restore,"secret",error))<<error;EXPECT_EQ(readText(restore+"/selected.txt"),"selected");EXPECT_FALSE(std::filesystem::exists(restore+"/sibling.txt"));
    std::atomic_bool cancelled{true};OperationContext context;context.cancelled=&cancelled;EXPECT_FALSE(repository.createSnapshot(options,info,error,context));
    
}

TEST(DirectoryWatcherTest, ReportsAFileSystemChange) {
    const std::string source=tempPath("watch");std::filesystem::create_directory(source);DirectoryWatcher watcher;std::mutex mutex;std::condition_variable changed;bool notified=false;std::string error;
    bool ready = false;
    ASSERT_TRUE(watcher.start(source,[&](const std::vector<std::string>&paths,bool){std::lock_guard<std::mutex>lock(mutex);notified=notified||!paths.empty();changed.notify_one();},error,50,500,
        [&](bool available,const std::string&){std::lock_guard<std::mutex>lock(mutex);ready=available;changed.notify_one();}))<<error;
    {std::unique_lock<std::mutex>lock(mutex);ASSERT_TRUE(changed.wait_for(lock,std::chrono::seconds(3),[&]{return ready;}));}
    writeText(source+"/change.txt","change");{std::unique_lock<std::mutex>lock(mutex);EXPECT_TRUE(changed.wait_for(lock,std::chrono::seconds(3),[&]{return notified;}));}watcher.stop();
}

TEST(DirectoryWatcherTest, ReopensAndRequestsFullRescanAfterDirectoryReturns) {
    const std::string source=tempPath("watch_replaced"), moved=source+"_old";
    std::filesystem::create_directory(source);
    DirectoryWatcher watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool initiallyAvailable=false, unavailable=false, restored=false, fullRescan=false;
    std::string error;
    ASSERT_TRUE(watcher.start(source,
        [&](const std::vector<std::string>&,bool full){
            if(full){std::lock_guard<std::mutex>lock(mutex);fullRescan=true;changed.notify_all();}
        },error,0,0,
        [&](bool available,const std::string&){
            std::lock_guard<std::mutex>lock(mutex);
            if(available){if(unavailable)restored=true;else initiallyAvailable=true;}
            else unavailable=true;
            changed.notify_all();
        }))<<error;
    {
        std::unique_lock<std::mutex>lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock,std::chrono::seconds(3),[&]{return initiallyAvailable;}));
    }
    ASSERT_TRUE((std::filesystem::rename(source, moved), true));
    {
        std::unique_lock<std::mutex>lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock,std::chrono::seconds(3),[&]{return unavailable;}));
    }
    ASSERT_TRUE(std::filesystem::create_directory(source));
    writeText(source+"/after-restart.txt","recreated");
    {
        std::unique_lock<std::mutex>lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock,std::chrono::seconds(5),[&]{return restored&&fullRescan;}));
    }
    watcher.stop();
    
    EXPECT_TRUE(restored);
    EXPECT_TRUE(fullRescan);
}

TEST(ArchiveV3Test, UsesAuthenticatedEncryptionAndSupportsCancellation) {
    const std::string source=tempPath("v3_source"), archive=tempPath("v3.abk"), restore=tempPath("v3_restore");
    std::filesystem::create_directory(source); writeText(source+"/secret.txt","authenticated archive");
    PackOptions options; options.password="correct"; std::string error;
    ASSERT_TRUE(ArchiveService::create(source,archive,options,error))<<error;
    std::ifstream input(archive,std::ios::binary); std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
    ASSERT_GT(bytes.size(),50u); EXPECT_EQ(std::string(bytes.begin(), bytes.begin()+8), "DBPK0001");
    bytes.back()^=0x01; std::ofstream changed(archive,std::ios::binary|std::ios::trunc); changed.write(reinterpret_cast<char*>(bytes.data()),bytes.size()); changed.close();
    EXPECT_FALSE(ArchiveService::extract(archive,restore,error,"correct"));
    std::atomic_bool cancelled{true}; OperationContext context; context.cancelled=&cancelled;
    EXPECT_FALSE(ArchiveService::create(std::vector<std::string>{source},archive,options,error,context));
    
}

TEST(ArchiveV3Test, PreservesSafeRelativeSymbolicLink) {
    const std::string source=tempPath("link_source"),archive=tempPath("link.abk"),restore=tempPath("link_restore");std::filesystem::create_directory(source);writeText(source+"/target.txt","target");
    std::filesystem::create_symlink("target.txt", source + "/shortcut.txt");
    PackOptions options;std::string error;ASSERT_TRUE(ArchiveService::create(source,archive,options,error))<<error;std::vector<ArchiveEntry>entries;ASSERT_TRUE(ArchiveService::inspect(archive,entries,error))<<error;auto link=std::find_if(entries.begin(),entries.end(),[](const ArchiveEntry&e){return e.type==2;});ASSERT_NE(link,entries.end());EXPECT_EQ(link->linkTarget,"target.txt");EXPECT_NE(link->mode, 0u);
    ASSERT_TRUE(ArchiveService::extract(archive,restore,error))<<error;char content[16]={};std::ifstream input(restore+"/shortcut.txt");input>>content;EXPECT_STREQ(content,"target");
}
TEST(LocalRepositoryTest, RejectsUnsafeSnapshotIdsBeforeFilesystemAccess) {
    const std::string root = tempPath("unsafe_id_repo"), destination = tempPath("unsafe_id_restore");
    LocalRepository repository(root); std::string error;
    EXPECT_FALSE(repository.restoreSnapshot("../../outside", destination, "secret", error));
    EXPECT_FALSE(repository.deleteSnapshot("../../outside", error));
    EXPECT_FALSE(std::filesystem::exists(tempPath("outside.manifest")));
    
}

TEST(LocalRepositoryTest, DoesNotOverwriteCorruptConfiguration) {
    const std::string root = tempPath("corrupt_config_repo");
    LocalRepository repository(root); std::string error;
    ASSERT_TRUE(repository.initialize("secret", error)) << error;
    const std::string config = root + "/config.bin";
    { std::ofstream file(config, std::ios::binary | std::ios::trunc); file << "broken config"; }
    EXPECT_FALSE(repository.initialize("new password", error));
    std::ifstream file(config, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "broken config");
    
}

TEST(LocalRepositoryTest, RejectsWrongPasswordForExistingSnapshots) {
    const std::string root = tempPath("wrong_password_repo"), source = tempPath("wrong_password_source"), restore = tempPath("wrong_password_restore");
    std::filesystem::create_directory(source); writeText(source + "/file.txt", "original");
    SnapshotOptions options; options.sources = {source}; options.password = "secret";
    LocalRepository repository(root); std::string error; SnapshotInfo first;
    ASSERT_TRUE(repository.createSnapshot(options, first, error)) << error;
    options.password = "wrong password"; LocalRepository wrongPassword(root); SnapshotInfo second;
    EXPECT_FALSE(wrongPassword.createSnapshot(options, second, error));
    ASSERT_TRUE(repository.restoreSnapshot(first.id, restore, "secret", error)) << error;
    EXPECT_EQ(readText(restore + "/" + source.substr(source.find_last_of("//") + 1) + "/file.txt"), "original");
    
}

TEST(LocalRepositoryTest, PreservesEmptySelectedDirectory) {
    const std::string root = tempPath("empty_dir_repo"), source = tempPath("empty_dir_source"), restore = tempPath("empty_dir_restore");
    std::filesystem::create_directory(source);
    SnapshotOptions options; options.sources = {source}; options.password = "secret";
    LocalRepository repository(root); std::string error; SnapshotInfo snapshot;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    ASSERT_TRUE(repository.restoreSnapshot(snapshot.id, restore, "secret", error)) << error;
    const std::string restoredRoot = restore + "/" + source.substr(source.find_last_of("//") + 1);
    EXPECT_TRUE(std::filesystem::is_directory(restoredRoot));
    
}

TEST(LocalRepositoryTest, RefusesPruneWhenRetainedManifestIsCorrupt) {
    const std::string root = tempPath("corrupt_prune_repo"), source = tempPath("corrupt_prune_source");
    std::filesystem::create_directory(source); writeText(source + "/file.txt", "content");
    SnapshotOptions options; options.sources = {source}; options.password = "secret";
    LocalRepository repository(root); std::string error; SnapshotInfo snapshot;
    ASSERT_TRUE(repository.createSnapshot(options, snapshot, error)) << error;
    { std::ofstream file(root + "/snapshots/" + snapshot.id + ".manifest", std::ios::binary | std::ios::trunc); file << "corrupt"; }
    EXPECT_FALSE(repository.prune(1, error));
    EXPECT_TRUE(std::filesystem::exists(root + "/snapshots/" + snapshot.id + ".manifest"));
    EXPECT_TRUE(std::filesystem::is_directory(root + "/chunks"));
    
}

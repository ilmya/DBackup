#include <gtest/gtest.h>
#include <algorithm>
#include <windows.h>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <string>

#include "automation/cron.h"
#include "automation/directory_watcher.h"
#include "repository/repository.h"

namespace {
std::string tempPath(const char *name) {
    char base[MAX_PATH]; GetTempPathA(MAX_PATH, base);
    return std::string(base) + "dbackup_s345_" + std::to_string(GetCurrentProcessId()) + "_" + name;
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
    std::time_t next=cron.next(std::mktime(&start));ASSERT_NE(next,static_cast<std::time_t>(-1));std::tm result={};localtime_s(&result,&next);EXPECT_EQ(result.tm_min,45);
}

TEST(LocalRepositoryTest, DeduplicatesAndRestoresEncryptedChunks) {
    const std::string root=tempPath("repo"), source=tempPath("source"), restore=tempPath("restore");
    CreateDirectoryA(source.c_str(),nullptr); writeText(source+"\\a.bin",std::string(8192,'A')); writeText(source+"\\b.bin",std::string(8192,'A'));
    LocalRepository repository(root); std::string error; ASSERT_TRUE(repository.initialize("secret",error))<<error;
    SnapshotOptions options; options.sources={source}; options.password="secret"; options.chunkSize=4096;
    SnapshotInfo first; ASSERT_TRUE(repository.createSnapshot(options,first,error))<<error; EXPECT_EQ(first.fileCount,2u);
    SnapshotInfo second; ASSERT_TRUE(repository.createSnapshot(options,second,error))<<error; EXPECT_EQ(second.storedBytes,0u);
    ASSERT_TRUE(repository.restoreSnapshot(second.id,restore,"secret",error))<<error;
    const std::string folder=restore+"\\"+source.substr(source.find_last_of("\\/")+1);
    EXPECT_EQ(readText(folder+"\\a.bin"),std::string(8192,'A'));
    EXPECT_FALSE(repository.restoreSnapshot(second.id,restore,"wrong",error));
    repository.prune(1,error);
    std::vector<SnapshotInfo> snapshots; ASSERT_TRUE(repository.listSnapshots(snapshots,error)); EXPECT_EQ(snapshots.size(),1u);
    system(("rmdir /s /q \""+root+"\" & rmdir /s /q \""+source+"\" & rmdir /s /q \""+restore+"\"").c_str());
}

TEST(LocalRepositoryTest, SingleFileSourceDoesNotIncludeItsSiblingsAndHonorsCancellation) {
    const std::string root=tempPath("single_repo"), source=tempPath("single_source"), restore=tempPath("single_restore");
    CreateDirectoryA(source.c_str(),nullptr);writeText(source+"\\selected.txt","selected");writeText(source+"\\sibling.txt","must not be copied");
    LocalRepository repository(root);std::string error;SnapshotOptions options;options.sources={source+"\\selected.txt"};options.password="secret";SnapshotInfo info;
    ASSERT_TRUE(repository.createSnapshot(options,info,error))<<error;EXPECT_EQ(info.fileCount,1u);ASSERT_TRUE(repository.restoreSnapshot(info.id,restore,"secret",error))<<error;EXPECT_EQ(readText(restore+"\\selected.txt"),"selected");EXPECT_EQ(GetFileAttributesA((restore+"\\sibling.txt").c_str()),INVALID_FILE_ATTRIBUTES);
    std::atomic_bool cancelled{true};OperationContext context;context.cancelled=&cancelled;EXPECT_FALSE(repository.createSnapshot(options,info,error,context));
    system(("rmdir /s /q \""+root+"\" & rmdir /s /q \""+source+"\" & rmdir /s /q \""+restore+"\"").c_str());
}

TEST(DirectoryWatcherTest, ReportsAFileSystemChange) {
    const std::string source=tempPath("watch");CreateDirectoryA(source.c_str(),nullptr);DirectoryWatcher watcher;std::mutex mutex;std::condition_variable changed;bool notified=false;std::string error;
    ASSERT_TRUE(watcher.start(source,[&](const std::vector<std::string>&paths,bool){std::lock_guard<std::mutex>lock(mutex);notified=notified||!paths.empty();changed.notify_one();},error,50,500))<<error;
    writeText(source+"\\change.txt","change");{std::unique_lock<std::mutex>lock(mutex);EXPECT_TRUE(changed.wait_for(lock,std::chrono::seconds(3),[&]{return notified;}));}watcher.stop();system(("rmdir /s /q \""+source+"\"").c_str());
}

TEST(ArchiveV3Test, UsesAuthenticatedEncryptionAndSupportsCancellation) {
    const std::string source=tempPath("v3_source"), archive=tempPath("v3.abk"), restore=tempPath("v3_restore");
    CreateDirectoryA(source.c_str(),nullptr); writeText(source+"\\secret.txt","authenticated archive");
    PackOptions options; options.password="correct"; std::string error;
    ASSERT_TRUE(Packer::pack(source,archive,options,error))<<error;
    std::ifstream input(archive,std::ios::binary); std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
    ASSERT_GT(bytes.size(),50u); EXPECT_EQ(bytes[6],3u);  // little-endian v3
    bytes.back()^=0x01; std::ofstream changed(archive,std::ios::binary|std::ios::trunc); changed.write(reinterpret_cast<char*>(bytes.data()),bytes.size()); changed.close();
    EXPECT_FALSE(Packer::unpack(archive,restore,error,"correct"));
    std::atomic_bool cancelled{true}; OperationContext context; context.cancelled=&cancelled;
    EXPECT_FALSE(Packer::pack(std::vector<std::string>{source},archive,options,error,context));
    system(("rmdir /s /q \""+source+"\" & rmdir /s /q \""+restore+"\" & del /q \""+archive+"\"").c_str());
}

TEST(ArchiveV3Test, PreservesSafeRelativeSymbolicLinkAndSecurityDescriptor) {
    const std::string source=tempPath("link_source"),archive=tempPath("link.abk"),restore=tempPath("link_restore");CreateDirectoryA(source.c_str(),nullptr);writeText(source+"\\target.txt","target");
    if(!CreateSymbolicLinkA((source+"\\shortcut.txt").c_str(),"target.txt",0))GTEST_SKIP()<<"Windows developer mode or symlink privilege is unavailable";
    PackOptions options;std::string error;ASSERT_TRUE(Packer::pack(source,archive,options,error))<<error;std::vector<ArchiveEntry>entries;ASSERT_TRUE(Packer::readArchive(archive,entries,error))<<error;auto link=std::find_if(entries.begin(),entries.end(),[](const ArchiveEntry&e){return e.type==2;});ASSERT_NE(link,entries.end());EXPECT_EQ(link->linkTarget,"target.txt");EXPECT_FALSE(link->securityDescriptor.empty());
    ASSERT_TRUE(Packer::unpack(archive,restore,error))<<error;char content[16]={};std::ifstream input(restore+"\\shortcut.txt");input>>content;EXPECT_STREQ(content,"target");system(("rmdir /s /q \""+source+"\" & rmdir /s /q \""+restore+"\" & del /q \""+archive+"\"").c_str());
}

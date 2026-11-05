#include <gtest/gtest.h>

#include <QDate>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <sys/stat.h>
#include <unistd.h>

#include "automation/job_manager.h"
#include "automation/plan_executor.h"
#include "core/repository.h"

namespace {
void clearJobsFile() {
    const QString directory = QDir::tempPath() + "/DBackupAutomationTests-" +
        QString::number(QCoreApplication::applicationPid());
    QDir(directory).removeRecursively();
    QDir().mkpath(directory);
}

QString jobsFilePath() {
    return QDir::tempPath() + "/DBackupAutomationTests-" +
        QString::number(QCoreApplication::applicationPid()) + "/jobs.json";
}

bool writeJobsDocument(const QJsonDocument &document) {
    QSaveFile file(jobsFilePath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(document.toJson()) < 0) return false;
    return file.commit();
}

BackupJob makeJob() {
    BackupJob job;
    job.id = "job-test";
    job.name = "Test backup";
    job.sources = {"/tmp/dbackup-test/source"};
    job.destination = "/tmp/dbackup-test/repository";
    job.cron = "* * * * *";
    return job;
}

class JobManagerTest : public ::testing::Test {
 protected:
    void SetUp() override { clearJobsFile(); }
    void TearDown() override { clearJobsFile(); }
};
}

TEST_F(JobManagerTest, InjectedClockRunsDueJobOnceAndAdvancesSchedule) {
    JobManager manager(nullptr, jobsFilePath());
    QDateTime now(QDate(2026, 10, 8), QTime(12, 0, 30), QTimeZone::systemTimeZone());
    manager.setClock([&now] { return now; });
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob &) { ++executions; });
    QString error;
    ASSERT_TRUE(manager.upsert(makeJob(), error)) << error.toStdString();
    ASSERT_EQ(manager.jobs().size(), 1);
    EXPECT_EQ(manager.jobs().first().nextRun.time(), QTime(12, 1));

    now = now.addSecs(30);
    manager.checkSchedules();
    EXPECT_EQ(executions, 1);
    EXPECT_EQ(manager.jobs().first().nextRun.time(), QTime(12, 2));
    manager.checkSchedules();
    EXPECT_EQ(executions, 1);
    manager.markFinished("job-test", true, "completed");
    EXPECT_EQ(manager.jobs().first().lastRun, now);
}

TEST_F(JobManagerTest, PersistsRealtimeOnlySelectionRulesAndPartialResult) {
    BackupJob job = makeJob();
    job.scheduled = false; job.realtime = true; job.cron.clear();
    job.filter.sourceRules = {{"/tmp/dbackup-test/source", true}, {"/tmp/dbackup-test/source/skip", false}};
    {
        JobManager manager(nullptr, jobsFilePath());
        QString error; ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
        manager.setExecutor([](const BackupJob &) {});
        ASSERT_TRUE(manager.runNow(job.id, error));
        manager.markFinished(job.id, true, "skipped file", true);
    }
    JobManager restored(nullptr, jobsFilePath());
    ASSERT_EQ(restored.jobs().size(), 1);
    const auto &loaded = restored.jobs().first();
    EXPECT_FALSE(loaded.scheduled); EXPECT_TRUE(loaded.realtime);
    EXPECT_FALSE(loaded.nextRun.isValid());
    ASSERT_EQ(loaded.filter.sourceRules.size(), 2u);
    EXPECT_TRUE(loaded.filter.sourceRules[0].include);
    EXPECT_FALSE(loaded.filter.sourceRules[1].include);
    EXPECT_TRUE(loaded.lastResult.contains("部分完成"));
}

TEST_F(JobManagerTest, PersistsManualArchivePlanAndRejectsRealtimeArchive) {
    auto job=makeJob(); job.scheduled=false; job.repository=false; job.encrypted=false;
    job.format=PackFormat::Tar; job.compression=Compression::None;
    QString error;
    { JobManager manager(nullptr,jobsFilePath()); ASSERT_TRUE(manager.upsert(job,error)) << error.toStdString(); }
    JobManager restored(nullptr,jobsFilePath()); ASSERT_EQ(restored.jobs().size(),1);
    const auto &loaded=restored.jobs().first();
    EXPECT_FALSE(loaded.repository); EXPECT_FALSE(loaded.encrypted); EXPECT_FALSE(loaded.scheduled);
    EXPECT_EQ(loaded.format,PackFormat::Tar); EXPECT_EQ(loaded.compression,Compression::None);
    job.realtime=true; EXPECT_FALSE(restored.upsert(job,error));
    EXPECT_FALSE(restored.jobs().first().realtime);
}

TEST_F(JobManagerTest, SaveFailureDoesNotCreateOrChangeAPlan) {
    JobManager manager(nullptr,jobsFilePath()+"/missing/jobs.json");
    QString error; EXPECT_FALSE(manager.upsert(makeJob(),error));
    EXPECT_FALSE(error.isEmpty()); EXPECT_TRUE(manager.jobs().isEmpty());
}

TEST_F(JobManagerTest, InvalidConfigurationIsReportedAndNeverOverwritten) {
    QFile file(jobsFilePath());ASSERT_TRUE(file.open(QIODevice::WriteOnly));file.write("invalid configuration");file.close();
    JobManager manager(nullptr,jobsFilePath());ASSERT_FALSE(manager.loadError().isEmpty());
    QString error;EXPECT_FALSE(manager.upsert(makeJob(),error));EXPECT_FALSE(error.isEmpty());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));EXPECT_EQ(file.readAll(),QByteArray("invalid configuration"));
}

TEST_F(JobManagerTest, ArchivePlanCreatesIndependentTarAndKeepsLatestOnSuccess) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath("source")));
    ASSERT_TRUE(QDir().mkpath(dir.filePath("output")));
    QFile source(dir.filePath("source/file")); ASSERT_TRUE(source.open(QIODevice::WriteOnly));source.write("data");source.close();
    auto job=makeJob();job.repository=false;job.encrypted=false;job.scheduled=false;job.format=PackFormat::Tar;
    job.compression=Compression::None;job.sources={dir.filePath("source")};job.destination=dir.filePath("output");job.retention=1;
    const auto first=executePlan(job,"");ASSERT_TRUE(first.success) << first.detail;ASSERT_FALSE(first.partial);
    QFile unrelated(dir.filePath("output/dbackup-job-test-personal.tar"));ASSERT_TRUE(unrelated.open(QIODevice::WriteOnly));unrelated.write("personal");unrelated.close();
    const auto second=executePlan(job,"");ASSERT_TRUE(second.success) << second.detail;
    EXPECT_NE(first.detail,second.detail);EXPECT_TRUE(QFile::exists(QString::fromStdString(second.detail)));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(first.detail)));
    EXPECT_TRUE(QFile::exists(unrelated.fileName()));
    std::vector<ArchiveEntry> entries;std::string error;
    EXPECT_TRUE(ArchiveService::inspect(second.detail,entries,error)) << error;
    job.sources={dir.filePath("missing")};
    EXPECT_FALSE(executePlan(job,"").success);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(second.detail)));
}

TEST_F(JobManagerTest, PartialArchivePlanDoesNotPrunePreviousArchive) {
    if(geteuid()==0) GTEST_SKIP() << "requires an unprivileged user";
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath("source")));ASSERT_TRUE(QDir().mkpath(dir.filePath("output")));
    QFile good(dir.filePath("source/good"));ASSERT_TRUE(good.open(QIODevice::WriteOnly));good.write("data");good.close();
    auto job=makeJob();job.repository=false;job.encrypted=false;job.scheduled=false;job.sources={dir.filePath("source")};job.destination=dir.filePath("output");job.retention=1;
    const auto first=executePlan(job,"");ASSERT_TRUE(first.success) << first.detail;
    QFile secret(dir.filePath("source/private"));ASSERT_TRUE(secret.open(QIODevice::WriteOnly));secret.write("secret");secret.close();
    ASSERT_EQ(chmod(secret.fileName().toUtf8().constData(),0000),0);
    const auto partial=executePlan(job,"");chmod(secret.fileName().toUtf8().constData(),0600);
    EXPECT_TRUE(partial.success) << partial.detail;EXPECT_TRUE(partial.partial);EXPECT_NE(partial.detail.find("private"),std::string::npos);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(first.detail)));
    EXPECT_EQ(QDir(job.destination).entryList({"*.abk"},QDir::Files).size(),2);
}

TEST_F(JobManagerTest, RealtimeJobWatchesSelectedSingleFile) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const QString selected = directory.filePath("file.txt");
    QFile file(selected); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("before"); file.close();
    BackupJob job = makeJob(); job.scheduled = false; job.realtime = true; job.cron.clear();
    job.sources = {selected}; job.destination = directory.filePath("repository");
    JobManager manager(nullptr, jobsFilePath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob &) { ++executions; });
    QString error; ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 4000 && manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents(); QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    ASSERT_TRUE(file.open(QIODevice::Append)); file.write("after"); file.close();
    timer.restart();
    while (timer.elapsed() < 7000 && executions == 0) { QCoreApplication::processEvents(); QThread::msleep(20); }
    EXPECT_EQ(executions, 1);
    if (executions) manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, RealtimeContinuesWhenSubdirectoryCannotBeRead) {
    if(geteuid()==0) GTEST_SKIP() << "requires an unprivileged user";
    QTemporaryDir directory;ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir().mkpath(directory.filePath("source/private")));
    const QString blocked=directory.filePath("source/private");ASSERT_EQ(chmod(blocked.toUtf8().constData(),0000),0);
    auto job=makeJob();job.scheduled=false;job.realtime=true;job.sources={directory.filePath("source")};job.destination=directory.filePath("repository");
    JobManager manager(nullptr,jobsFilePath());int executions=0;manager.setExecutor([&](const BackupJob &){++executions;});
    QString error;ASSERT_TRUE(manager.upsert(job,error)) << error.toStdString();manager.start();
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<4000 && !manager.jobs().first().realtimeStatus.contains(QString::fromUtf8("部分目录无法监控"))){QCoreApplication::processEvents();QThread::msleep(20);}
    EXPECT_TRUE(manager.jobs().first().realtimeStatus.contains(QString::fromUtf8("private")));
    QFile file(directory.filePath("source/change"));ASSERT_TRUE(file.open(QIODevice::WriteOnly));file.write("changed");file.close();
    timer.restart();while(timer.elapsed()<7000 && !executions){QCoreApplication::processEvents();QThread::msleep(20);}
    EXPECT_EQ(executions,1);chmod(blocked.toUtf8().constData(),0700);
}

TEST_F(JobManagerTest, RealtimeJobBacksUpSelectedSocket) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const QString selected = directory.filePath("endpoint.sock");
    ASSERT_EQ(mknod(selected.toUtf8().constData(), S_IFSOCK | 0600, 0), 0);
    BackupJob job = makeJob(); job.scheduled = false; job.realtime = true; job.cron.clear();
    job.sources = {selected}; job.destination = directory.filePath("repository");
    LocalRepository repo(job.destination.toStdString());
    std::string detail; ASSERT_TRUE(repo.initialize("test-only", detail)) << detail;
    JobManager manager(nullptr, jobsFilePath());
    SnapshotInfo snapshot; int executions = 0; bool succeeded = false;
    manager.setExecutor([&](const BackupJob &requested) {
        ++executions; SnapshotOptions options; options.password = "test-only";
        for (const auto &source : requested.sources) options.sources.push_back(source.toStdString());
        succeeded = repo.createSnapshot(options, snapshot, detail);
    });
    QString error; ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 4000 && manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents(); QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    ASSERT_EQ(unlink(selected.toUtf8().constData()), 0);
    ASSERT_EQ(mknod(selected.toUtf8().constData(), S_IFSOCK | 0600, 0), 0);
    timer.restart();
    while (timer.elapsed() < 7000 && executions == 0) { QCoreApplication::processEvents(); QThread::msleep(20); }
    ASSERT_EQ(executions, 1); ASSERT_TRUE(succeeded) << detail;
    const auto output = directory.filePath("restored");
    ASSERT_TRUE(repo.restoreSnapshot(snapshot.id, output.toStdString(), "test-only", detail)) << detail;
    struct stat info{}; ASSERT_EQ(lstat((output + "/endpoint.sock").toUtf8().constData(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, RepeatedManualRequestsQueueOnlyOneRerun) {
    JobManager manager(nullptr, jobsFilePath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob &) { ++executions; });
    QString error;
    ASSERT_TRUE(manager.upsert(makeJob(), error)) << error.toStdString();

    ASSERT_TRUE(manager.runNow("job-test", error)) << error.toStdString();
    EXPECT_EQ(executions, 1);
    ASSERT_TRUE(manager.runNow("job-test", error));
    EXPECT_NE(error.indexOf("排队"), -1);
    ASSERT_TRUE(manager.runNow("job-test", error));
    EXPECT_EQ(executions, 1);

    manager.markFinished("job-test", true, "first run");
    EXPECT_EQ(executions, 2);
    manager.markFinished("job-test", true, "queued run");
}

TEST_F(JobManagerTest, RemovingRunningJobDoesNotExecuteQueuedOrphan) {
    JobManager manager(nullptr, jobsFilePath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob &) { ++executions; });
    QString error;
    ASSERT_TRUE(manager.upsert(makeJob(), error)) << error.toStdString();
    ASSERT_TRUE(manager.runNow("job-test", error));
    ASSERT_TRUE(manager.runNow("job-test", error));

    ASSERT_TRUE(manager.remove("job-test", error)) << error.toStdString();
    manager.markFinished("job-test", true, "removed while running");
    EXPECT_EQ(executions, 1);
    EXPECT_TRUE(manager.jobs().isEmpty());
}

TEST_F(JobManagerTest, DisablingJobCancelsPendingRerunAndAllowsReenable) {
    JobManager manager(nullptr, jobsFilePath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob &) { ++executions; });
    QString error;
    ASSERT_TRUE(manager.upsert(makeJob(), error)) << error.toStdString();
    ASSERT_TRUE(manager.runNow("job-test", error));
    ASSERT_TRUE(manager.runNow("job-test", error));
    ASSERT_TRUE(manager.setEnabled("job-test", false, error)) << error.toStdString();
    EXPECT_FALSE(manager.jobs().first().enabled);
    EXPECT_FALSE(manager.jobs().first().nextRun.isValid());

    manager.markFinished("job-test", true, "completed");
    EXPECT_EQ(executions, 1);
    EXPECT_FALSE(manager.jobs().first().nextRun.isValid());
    ASSERT_TRUE(manager.setEnabled("job-test", true, error)) << error.toStdString();
    EXPECT_TRUE(manager.jobs().first().enabled);
    EXPECT_TRUE(manager.jobs().first().nextRun.isValid());
}

TEST_F(JobManagerTest, RejectsLegacyAndIncompleteJobConfiguration) {
    {
        JobManager writer(nullptr, jobsFilePath());
        QString error;
        ASSERT_TRUE(writer.upsert(makeJob(), error)) << error.toStdString();
    }

    QFile file(jobsFilePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << jobsFilePath().toStdString();
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    ASSERT_TRUE(document.isObject());

    QJsonObject root = document.object();
    root["version"] = 1;
    ASSERT_TRUE(writeJobsDocument(QJsonDocument(root)));
    {
        JobManager legacy(nullptr, jobsFilePath());
        EXPECT_TRUE(legacy.jobs().isEmpty());
    }

    root["version"] = 2;
    QJsonArray jobs = root.value("jobs").toArray();
    ASSERT_FALSE(jobs.isEmpty());
    QJsonObject job = jobs.first().toObject();
    job.remove("id");
    jobs[0] = job;
    root["jobs"] = jobs;
    ASSERT_TRUE(writeJobsDocument(QJsonDocument(root)));
    {
        JobManager incomplete(nullptr, jobsFilePath());
        EXPECT_TRUE(incomplete.jobs().isEmpty());
    }
}

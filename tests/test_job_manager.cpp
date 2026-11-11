#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <sys/stat.h>
#include <unistd.h>

#include "automation/cron.h"
#include "automation/directory_watcher.h"
#include "automation/job_manager.h"
#include "automation/plan_executor.h"
#include "core/repository.h"
#include "core/incremental_chain.h"
#include "test_support.h"

namespace {
bool writeJobsDocument(const QString& path, const QJsonDocument& document) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(document.toJson()) < 0)
        return false;
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
    void SetUp() override {
        ASSERT_TRUE(directory_.isValid());
    }

    QString configPath() const {
        return directory_.filePath("jobs.json");
    }
    bool writeConfig(const QJsonDocument& document) const {
        return writeJobsDocument(configPath(), document);
    }

  private:
    QTemporaryDir directory_;
};
} // namespace

TEST_F(JobManagerTest, InjectedClockRunsDueJobOnceAndAdvancesSchedule) {
    JobManager manager(nullptr, configPath());
    QDateTime now(QDate(2026, 10, 8), QTime(12, 0, 30), QTimeZone::systemTimeZone());
    manager.setClock([&now] { return now; });
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob&) { ++executions; });
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

TEST_F(JobManagerTest, RemembersLastCompleteSuccessAcrossFailuresAndReload) {
    QDateTime now(QDate(2026, 10, 8), QTime(12, 0), QTimeZone::systemTimeZone());
    QString error;
    {
        JobManager manager(nullptr, configPath());
        manager.setClock([&now] { return now; });
        ASSERT_TRUE(manager.upsert(makeJob(), error));
        manager.markFinished("job-test", true, "complete");
        const auto success = now;
        EXPECT_EQ(manager.jobs().first().lastSuccess, success);
        now = now.addSecs(60);
        manager.markFinished("job-test", true, "partial", true);
        EXPECT_EQ(manager.jobs().first().lastSuccess, success);
        manager.markFinished("job-test", false, "failed");
        EXPECT_EQ(manager.jobs().first().lastSuccess, success);
        manager.markFinished("job-test", false, "cancelled", false, true);
        EXPECT_EQ(manager.jobs().first().lastSuccess, success);
    }
    JobManager restored(nullptr, configPath());
    ASSERT_EQ(restored.jobs().size(), 1);
    EXPECT_EQ(restored.jobs().first().lastSuccess, now.addSecs(-60));
}

TEST_F(JobManagerTest, PersistsRealtimeOnlySelectionRulesAndPartialResult) {
    BackupJob job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.cron.clear();
    job.filter.sourceRules = {{"/tmp/dbackup-test/source", true},
                              {"/tmp/dbackup-test/source/skip", false}};
    job.filter.typeMask = 0x15;
    {
        JobManager manager(nullptr, configPath());
        QString error;
        ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
        manager.setExecutor([](const BackupJob&) {});
        ASSERT_TRUE(manager.runNow(job.id, error));
        manager.markFinished(job.id, true, "skipped file", true);
    }
    JobManager restored(nullptr, configPath());
    ASSERT_EQ(restored.jobs().size(), 1);
    const auto& loaded = restored.jobs().first();
    EXPECT_FALSE(loaded.scheduled);
    EXPECT_TRUE(loaded.realtime);
    EXPECT_FALSE(loaded.nextRun.isValid());
    ASSERT_EQ(loaded.filter.sourceRules.size(), 2u);
    EXPECT_TRUE(loaded.filter.sourceRules[0].include);
    EXPECT_FALSE(loaded.filter.sourceRules[1].include);
    EXPECT_EQ(loaded.filter.typeMask, 0x15u);
    EXPECT_TRUE(loaded.lastResult.contains("部分完成"));
}

TEST_F(JobManagerTest, PersistsManualArchivePlanAndRejectsRealtimeArchive) {
    auto job = makeJob();
    job.scheduled = false;
    job.repository = false;
    job.encrypted = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    QString error;
    {
        JobManager manager(nullptr, configPath());
        ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    }
    JobManager restored(nullptr, configPath());
    ASSERT_EQ(restored.jobs().size(), 1);
    const auto& loaded = restored.jobs().first();
    EXPECT_FALSE(loaded.repository);
    EXPECT_FALSE(loaded.encrypted);
    EXPECT_FALSE(loaded.scheduled);
    EXPECT_EQ(loaded.format, PackFormat::Tar);
    EXPECT_EQ(loaded.compression, Compression::None);
    job.realtime = true;
    EXPECT_FALSE(restored.upsert(job, error));
    EXPECT_FALSE(restored.jobs().first().realtime);
}

TEST_F(JobManagerTest, PersistsAndExecutesRealtimeTarIncrementalPlan) {
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    ASSERT_TRUE(QDir().mkpath(data.filePath("source")));
    QFile source(data.filePath("source/file"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("first"), 5);
    source.close();
    auto job = makeJob();
    job.sources = {data.filePath("source")};
    job.destination = data.path();
    job.repository = false;
    job.tarIncremental = true;
    job.encrypted = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    job.scheduled = false;
    job.realtime = true;
    QString error;
    {
        JobManager manager(nullptr, configPath());
        ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    }
    JobManager restored(nullptr, configPath());
    ASSERT_EQ(restored.jobs().size(), 1);
    const auto loaded = restored.jobs().first();
    EXPECT_TRUE(loaded.tarIncremental);
    EXPECT_TRUE(loaded.realtime);
    EXPECT_FALSE(loaded.repository);
    const auto first = executePlan(loaded, "");
    ASSERT_TRUE(first.success) << first.detail;
    ASSERT_TRUE(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(source.write("second"), 6);
    source.close();
    const auto second = executePlan(loaded, "");
    ASSERT_TRUE(second.success) << second.detail;
    const auto chain = QDir(job.destination).filePath("dbackup-" + job.id + "-tar-incremental");
    std::vector<dbackup::incremental::VersionInfo> versions;
    std::string detail;
    ASSERT_TRUE(dbackup::incremental::listVersions(chain.toStdString(), "", {}, versions, detail))
        << detail;
    ASSERT_EQ(versions.size(), 2u);
    EXPECT_TRUE(versions[0].full);
    EXPECT_FALSE(versions[1].full);
}

TEST_F(JobManagerTest, PersistsAndExecutesRealtimeCustomIncrementalPlan) {
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    ASSERT_TRUE(QDir().mkpath(data.filePath("source")));
    QFile source(data.filePath("source/file"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("first"), 5);
    source.close();
    auto job = makeJob();
    job.sources = {data.filePath("source")};
    job.destination = data.path();
    job.repository = false;
    job.tarIncremental = true;
    job.encrypted = false;
    job.format = PackFormat::Custom;
    job.compression = Compression::None;
    job.scheduled = false;
    job.realtime = true;
    QString error;
    { JobManager manager(nullptr, configPath()); ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString(); }
    JobManager restored(nullptr, configPath());
    ASSERT_EQ(restored.jobs().size(), 1);
    const auto loaded = restored.jobs().first();
    EXPECT_EQ(loaded.format, PackFormat::Custom);
    const auto result = executePlan(loaded, "");
    ASSERT_TRUE(result.success) << result.detail;
    const auto chain = QDir(job.destination).filePath("dbackup-" + job.id + "-tar-incremental");
    std::vector<dbackup::incremental::VersionInfo> versions;
    std::string detail;
    ASSERT_TRUE(dbackup::incremental::listVersions(chain.toStdString(), "", {}, versions, detail)) << detail;
    ASSERT_EQ(versions.size(), 1u);
    ASSERT_TRUE(dbackup::incremental::restoreVersion(chain.toStdString(), versions[0].sequence, "",
                data.filePath("restored").toStdString(), {}, detail)) << detail;
    QFile output(data.filePath("restored/source/file"));
    ASSERT_TRUE(output.open(QIODevice::ReadOnly));
    EXPECT_EQ(output.readAll(), QByteArray("first"));
}

TEST_F(JobManagerTest, ValidatesPlanFieldsAndRejectsUnsafeIdentifiers) {
    JobManager manager(nullptr, configPath());
    QString error;
    const BackupJob valid = makeJob();
    ASSERT_TRUE(manager.upsert(valid, error)) << error.toStdString();

    auto invalid = valid;
    invalid.repository = false;
    invalid.realtime = true;
    EXPECT_FALSE(manager.upsert(invalid, error));

    invalid = valid;
    invalid.encrypted = false;
    EXPECT_FALSE(manager.upsert(invalid, error));

    invalid = valid;
    invalid.cron = QStringLiteral("61 * * * *");
    EXPECT_FALSE(manager.upsert(invalid, error));

    invalid = valid;
    invalid.name.clear();
    EXPECT_FALSE(manager.upsert(invalid, error));
    invalid = valid;
    invalid.sources.clear();
    EXPECT_FALSE(manager.upsert(invalid, error));
    invalid = valid;
    invalid.destination.clear();
    EXPECT_FALSE(manager.upsert(invalid, error));
    invalid = valid;
    invalid.retention = 0;
    EXPECT_FALSE(manager.upsert(invalid, error));

    invalid = valid;
    invalid.filter.minSize = 20;
    invalid.filter.maxSize = 10;
    EXPECT_FALSE(manager.upsert(invalid, error));
    invalid = valid;
    invalid.format = static_cast<PackFormat>(2);
    EXPECT_FALSE(manager.upsert(invalid, error));
    invalid = valid;
    invalid.compression = static_cast<Compression>(4);
    EXPECT_FALSE(manager.upsert(invalid, error));

    for (const QString& id :
         {QStringLiteral("../job"), QStringLiteral("job_name"), QString::fromUtf8("任务")}) {
        invalid = valid;
        invalid.id = id;
        EXPECT_FALSE(manager.upsert(invalid, error)) << id.toStdString();
    }
    EXPECT_EQ(manager.jobs().size(), 1);
    EXPECT_EQ(manager.jobs().first().id, valid.id);
}

TEST_F(JobManagerTest, SaveFailureDoesNotCreateOrChangeAPlan) {
    JobManager manager(nullptr, configPath() + "/missing/jobs.json");
    QString error;
    EXPECT_FALSE(manager.upsert(makeJob(), error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(manager.jobs().isEmpty());
}

TEST_F(JobManagerTest, InvalidConfigurationIsReportedAndNeverOverwritten) {
    QFile file(configPath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("invalid configuration");
    file.close();
    JobManager manager(nullptr, configPath());
    ASSERT_FALSE(manager.loadError().isEmpty());
    QString error;
    EXPECT_FALSE(manager.upsert(makeJob(), error));
    EXPECT_FALSE(error.isEmpty());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("invalid configuration"));
}

TEST_F(JobManagerTest, ArchivePlanCreatesIndependentTarAndKeepsLatestOnSuccess) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath("source")));
    ASSERT_TRUE(QDir().mkpath(dir.filePath("output")));
    QFile source(dir.filePath("source/file"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("data");
    source.close();
    auto job = makeJob();
    job.repository = false;
    job.encrypted = false;
    job.scheduled = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    job.sources = {dir.filePath("source")};
    job.destination = dir.filePath("output");
    job.retention = 1;
    const auto first = executePlan(job, "");
    ASSERT_TRUE(first.success) << first.detail;
    ASSERT_FALSE(first.partial);
    QFile unrelated(dir.filePath("output/dbackup-job-test-personal.tar"));
    ASSERT_TRUE(unrelated.open(QIODevice::WriteOnly));
    unrelated.write("personal");
    unrelated.close();
    const auto second = executePlan(job, "");
    ASSERT_TRUE(second.success) << second.detail;
    EXPECT_NE(first.detail, second.detail);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(second.detail)));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(first.detail)));
    EXPECT_TRUE(QFile::exists(unrelated.fileName()));
    std::vector<ArchiveEntry> entries;
    std::string error;
    EXPECT_TRUE(ArchiveService::inspect(second.detail, entries, error)) << error;
    job.sources = {dir.filePath("missing")};
    EXPECT_FALSE(executePlan(job, "").success);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(second.detail)));
}

TEST_F(JobManagerTest, PartialArchivePlanDoesNotPrunePreviousArchive) {
    if (geteuid() == 0)
        GTEST_SKIP() << "requires an unprivileged user";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath("source")));
    ASSERT_TRUE(QDir().mkpath(dir.filePath("output")));
    QFile good(dir.filePath("source/good"));
    ASSERT_TRUE(good.open(QIODevice::WriteOnly));
    good.write("data");
    good.close();
    auto job = makeJob();
    job.repository = false;
    job.encrypted = false;
    job.scheduled = false;
    job.sources = {dir.filePath("source")};
    job.destination = dir.filePath("output");
    job.retention = 1;
    const auto first = executePlan(job, "");
    ASSERT_TRUE(first.success) << first.detail;
    QFile secret(dir.filePath("source/private"));
    ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
    secret.write("secret");
    secret.close();
    ASSERT_EQ(chmod(secret.fileName().toUtf8().constData(), 0000), 0);
    const auto partial = executePlan(job, "");
    chmod(secret.fileName().toUtf8().constData(), 0600);
    EXPECT_TRUE(partial.success) << partial.detail;
    EXPECT_TRUE(partial.partial);
    EXPECT_NE(partial.detail.find("private"), std::string::npos);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(first.detail)));
    EXPECT_EQ(QDir(job.destination).entryList({"*.abk"}, QDir::Files).size(), 2);
}

TEST_F(JobManagerTest, RebuildingRealtimePlanRequestsRescanAndHonorsPause) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir().mkpath(directory.filePath("source")));
    auto job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.sources = {directory.filePath("source")};
    job.destination = directory.filePath("repository");
    JobManager manager(nullptr, configPath());
    QList<BackupJob> executions;
    manager.setExecutor([&](const BackupJob& value) { executions.append(value); });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.pause(true);
    job.name = "edited";
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    EXPECT_TRUE(executions.empty());
    manager.pause(false);
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 4000 && executions.empty()) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(executions.size(), 1);
    EXPECT_TRUE(executions.first().fullRescan);
    EXPECT_TRUE(executions.first().realtimeTriggered);
    manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, EditingAnotherPlanKeepsPendingRealtimeChange) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir().mkpath(directory.filePath("source")));
    auto job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.sources = {directory.filePath("source")};
    job.destination = directory.filePath("repository");
    JobManager manager(nullptr, configPath());
    int executions = 0;
    BackupJob requested;
    manager.setExecutor([&](const BackupJob& value) { ++executions; requested = value; });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 && manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    QFile file(directory.filePath("source/change.txt"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("changed");
    file.close();
    timer.restart();
    while (timer.elapsed() < 500) { QCoreApplication::processEvents(); QThread::msleep(20); }
    auto other = makeJob();
    other.id = "other-job";
    other.scheduled = false;
    ASSERT_TRUE(manager.upsert(other, error)) << error.toStdString();
    timer.restart();
    while (timer.elapsed() < 7000 && !executions) { QCoreApplication::processEvents(); QThread::msleep(20); }
    ASSERT_EQ(executions, 1);
    EXPECT_TRUE(requested.realtimeTriggered);
    EXPECT_TRUE(requested.changedPaths.contains(file.fileName()));
    manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, RealtimeJobWatchesSelectedSingleFile) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString selected = directory.filePath("file.txt");
    QFile file(selected);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("before");
    file.close();
    BackupJob job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.cron.clear();
    job.sources = {selected};
    job.destination = directory.filePath("repository");
    JobManager manager(nullptr, configPath());
    int executions = 0;
    BackupJob requested;
    manager.setExecutor([&](const BackupJob& value) {
        ++executions;
        requested = value;
    });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 &&
           manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    ASSERT_TRUE(file.open(QIODevice::Append));
    file.write("after");
    file.close();
    timer.restart();
    while (timer.elapsed() < 7000 && executions == 0) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    EXPECT_EQ(executions, 1);
    EXPECT_TRUE(requested.realtimeTriggered);
    EXPECT_TRUE(requested.changedPaths.contains(selected));
    if (executions)
        manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, RealtimeChangeCreatesTarIncrementAndRestoresIt) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString selected = directory.filePath("source.txt");
    QFile file(selected);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("before"), 6);
    file.close();
    auto job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.repository = false;
    job.tarIncremental = true;
    job.encrypted = false;
    job.format = PackFormat::Tar;
    job.compression = Compression::None;
    job.sources = {selected};
    job.destination = directory.path();
    const auto initial = executePlan(job, "");
    ASSERT_TRUE(initial.success) << initial.detail;

    JobManager manager(nullptr, configPath());
    int changes = 0;
    PlanResult result;
    manager.setExecutor([&](const BackupJob& run) {
        result = executePlan(run, "");
        if (run.changedPaths.contains(selected))
            ++changes;
        manager.markFinished(run.id, result.success, QString::fromStdString(result.detail), result.partial);
    });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 &&
           manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write("after"), 5);
    file.close();
    timer.restart();
    while (timer.elapsed() < 10000 && !changes) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_GT(changes, 0);
    ASSERT_TRUE(result.success) << result.detail;
    const QString chain = QDir(job.destination).filePath("dbackup-" + job.id + "-tar-incremental");
    std::vector<dbackup::incremental::VersionInfo> versions;
    std::string detail;
    ASSERT_TRUE(dbackup::incremental::listVersions(chain.toStdString(), "", {}, versions, detail))
        << detail;
    ASSERT_GE(versions.size(), 2u);
    const auto restored = directory.filePath("restored");
    ASSERT_TRUE(dbackup::incremental::restoreVersion(chain.toStdString(), versions.back().sequence,
                                                      "", restored.toStdString(), {}, detail)) << detail;
    QFile restoredFile(QDir(restored).filePath("source.txt"));
    ASSERT_TRUE(restoredFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(restoredFile.readAll(), QByteArray("after"));
}

TEST_F(JobManagerTest, RealtimeContinuesWhenSubdirectoryCannotBeRead) {
    if (geteuid() == 0)
        GTEST_SKIP() << "requires an unprivileged user";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir().mkpath(directory.filePath("source/private")));
    const QString blocked = directory.filePath("source/private");
    ASSERT_EQ(chmod(blocked.toUtf8().constData(), 0000), 0);
    auto job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.sources = {directory.filePath("source")};
    job.destination = directory.filePath("repository");
    JobManager manager(nullptr, configPath());
    int executions = 0;
    manager.setExecutor([&](const BackupJob&) { ++executions; });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 &&
           !manager.jobs().first().realtimeStatus.contains(QString::fromUtf8("部分目录无法监控"))) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    EXPECT_TRUE(manager.jobs().first().realtimeStatus.contains(QString::fromUtf8("private")));
    QFile file(directory.filePath("source/change"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("changed");
    file.close();
    timer.restart();
    while (timer.elapsed() < 7000 && !executions) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    EXPECT_EQ(executions, 1);
    chmod(blocked.toUtf8().constData(), 0700);
}

TEST_F(JobManagerTest, RealtimeJobBacksUpSelectedSocket) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString selected = directory.filePath("endpoint.sock");
    ASSERT_EQ(mknod(selected.toUtf8().constData(), S_IFSOCK | 0600, 0), 0);
    BackupJob job = makeJob();
    job.scheduled = false;
    job.realtime = true;
    job.cron.clear();
    job.sources = {selected};
    job.destination = directory.filePath("repository");
    LocalRepository repo(job.destination.toStdString());
    std::string detail;
    ASSERT_TRUE(repo.initialize("test-only", detail)) << detail;
    JobManager manager(nullptr, configPath());
    SnapshotInfo snapshot;
    int executions = 0;
    bool succeeded = false;
    manager.setExecutor([&](const BackupJob& requested) {
        ++executions;
        SnapshotOptions options;
        options.password = "test-only";
        for (const auto& source : requested.sources)
            options.sources.push_back(source.toStdString());
        succeeded = repo.createSnapshot(options, snapshot, detail);
    });
    QString error;
    ASSERT_TRUE(manager.upsert(job, error)) << error.toStdString();
    manager.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 &&
           manager.jobs().first().realtimeStatus != QString::fromUtf8("监控中")) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(manager.jobs().first().realtimeStatus, QString::fromUtf8("监控中"));
    ASSERT_EQ(unlink(selected.toUtf8().constData()), 0);
    ASSERT_EQ(mknod(selected.toUtf8().constData(), S_IFSOCK | 0600, 0), 0);
    timer.restart();
    while (timer.elapsed() < 7000 && executions == 0) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    ASSERT_EQ(executions, 1);
    ASSERT_TRUE(succeeded) << detail;
    const auto output = directory.filePath("restored");
    ASSERT_TRUE(repo.restoreSnapshot(snapshot.id, output.toStdString(), "test-only", detail))
        << detail;
    struct stat info {};
    ASSERT_EQ(lstat((output + "/endpoint.sock").toUtf8().constData(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    manager.markFinished(job.id, true, "done");
}

TEST_F(JobManagerTest, RepeatedManualRequestsQueueOnlyOneRerun) {
    JobManager manager(nullptr, configPath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob&) { ++executions; });
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
    JobManager manager(nullptr, configPath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob&) { ++executions; });
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
    JobManager manager(nullptr, configPath());
    int executions = 0;
    manager.setExecutor([&executions](const BackupJob&) { ++executions; });
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
        JobManager writer(nullptr, configPath());
        QString error;
        ASSERT_TRUE(writer.upsert(makeJob(), error)) << error.toStdString();
    }

    QFile file(configPath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << configPath().toStdString();
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    ASSERT_TRUE(document.isObject());

    QJsonObject root = document.object();
    root["version"] = 1;
    ASSERT_TRUE(writeConfig(QJsonDocument(root)));
    {
        JobManager legacy(nullptr, configPath());
        EXPECT_TRUE(legacy.jobs().isEmpty());
    }

    root["version"] = 2;
    QJsonArray jobs = root.value("jobs").toArray();
    ASSERT_FALSE(jobs.isEmpty());
    QJsonObject job = jobs.first().toObject();
    job.remove("id");
    jobs[0] = job;
    root["jobs"] = jobs;
    ASSERT_TRUE(writeConfig(QJsonDocument(root)));
    {
        JobManager incomplete(nullptr, configPath());
        EXPECT_TRUE(incomplete.jobs().isEmpty());
    }
}

TEST(DirectoryWatcherTest, ReportsAFileSystemChange) {
    test_support::TemporaryDirectory temporary("dbackup-watcher");
    const std::string source = (temporary.path() / "watch").string();
    std::filesystem::create_directory(source);
    DirectoryWatcher watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool notified = false;
    std::string error;
    bool ready = false;
    ASSERT_TRUE(watcher.start(
        source,
        [&](const std::vector<std::string>& paths, bool) {
            std::lock_guard<std::mutex> lock(mutex);
            notified = notified || !paths.empty();
            changed.notify_one();
        },
        error, 50, 500,
        [&](bool available, const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            ready = available;
            changed.notify_one();
        }))
        << error;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return ready; }));
    }
    test_support::writeText(std::filesystem::path(source) / "change.txt", "change");
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return notified; }));
    }
    watcher.stop();
}

TEST(DirectoryWatcherTest, ObservesCreateModifyRenameAndDelete) {
    test_support::TemporaryDirectory temporary("dbackup-watcher-events");
    const auto root = temporary.path() / "watch";
    std::filesystem::create_directory(root);
    DirectoryWatcher watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    std::set<std::string> observed;
    std::string error;
    ASSERT_TRUE(watcher.start(root.string(), [&](const std::vector<std::string>& paths, bool) {
        std::lock_guard<std::mutex> lock(mutex);
        observed.insert(paths.begin(), paths.end());
        changed.notify_all();
    }, error, 20, 500, [&](bool available, const std::string&) {
        std::lock_guard<std::mutex> lock(mutex);
        ready = available;
        changed.notify_all();
    })) << error;
    auto waitFor = [&](const std::string& name) {
        std::unique_lock<std::mutex> lock(mutex);
        bool found = changed.wait_for(lock, std::chrono::seconds(3), [&] { return observed.count(name) != 0; });
        observed.clear();
        return found;
    };
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return ready; }));
    }
    test_support::writeText(root / "first", "created");
    ASSERT_TRUE(waitFor("first"));
    test_support::writeText(root / "first", "modified");
    ASSERT_TRUE(waitFor("first"));
    std::filesystem::rename(root / "first", root / "second");
    ASSERT_TRUE(waitFor("second"));
    std::filesystem::remove(root / "second");
    ASSERT_TRUE(waitFor("second"));
    watcher.stop();
}

TEST(DirectoryWatcherTest, ReopensAndRequestsFullRescanAfterDirectoryReturns) {
    test_support::TemporaryDirectory temporary("dbackup-watcher-replaced");
    const std::string source = (temporary.path() / "watch").string(), moved = source + "_old";
    std::filesystem::create_directory(source);
    DirectoryWatcher watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool initiallyAvailable = false, unavailable = false, restored = false, fullRescan = false;
    std::string error;
    ASSERT_TRUE(watcher.start(
        source,
        [&](const std::vector<std::string>&, bool full) {
            if (full) {
                std::lock_guard<std::mutex> lock(mutex);
                fullRescan = true;
                changed.notify_all();
            }
        },
        error, 0, 0,
        [&](bool available, const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            if (available) {
                if (unavailable)
                    restored = true;
                else
                    initiallyAvailable = true;
            } else
                unavailable = true;
            changed.notify_all();
        }))
        << error;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(
            changed.wait_for(lock, std::chrono::seconds(3), [&] { return initiallyAvailable; }));
    }
    ASSERT_TRUE((std::filesystem::rename(source, moved), true));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return unavailable; }));
    }
    ASSERT_TRUE(std::filesystem::create_directory(source));
    test_support::writeText(std::filesystem::path(source) / "after-restart.txt", "recreated");
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return restored && fullRescan; }));
    }
    watcher.stop();

    EXPECT_TRUE(restored);
    EXPECT_TRUE(fullRescan);
}

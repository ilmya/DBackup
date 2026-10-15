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

#include "automation/job_manager.h"

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
    job.sources = {"C:/dbackup-test/source"};
    job.destination = "C:/dbackup-test/repository";
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
    job.remove("repositoryType");
    jobs[0] = job;
    root["jobs"] = jobs;
    ASSERT_TRUE(writeJobsDocument(QJsonDocument(root)));
    {
        JobManager incomplete(nullptr, jobsFilePath());
        EXPECT_TRUE(incomplete.jobs().isEmpty());
    }
}

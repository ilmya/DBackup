// 验证计划请求与排队策略
#include "automation/job_manager.h"
#include "automation/schedule/cron.h"
#include <gtest/gtest.h>
#include <cstdlib>

namespace {
BackupJob validJob() {
    BackupJob job;
    job.id = "job-1";
    job.name = "Backup";
    job.sources = {"/source"};
    job.destination = "/backup-output";
    job.cron = "* * * * *";
    return job;
}
void invalid(const BackupJob& job) {
    QString error;
    EXPECT_FALSE(job_policy::validate(job, error));
    EXPECT_FALSE(error.isEmpty());
}
}
TEST(JobPolicy, AcceptsValidPlan) {
    QString error = "old";
    EXPECT_TRUE(job_policy::validate(validJob(), error));
    EXPECT_TRUE(error.isEmpty());
}
TEST(JobPolicy, RejectsEmptyName) { auto job = validJob(); job.name = "  "; invalid(job); }
TEST(JobPolicy, RejectsEmptySources) { auto job = validJob(); job.sources.clear(); invalid(job); }
TEST(JobPolicy, RejectsEmptyDestination) { auto job = validJob(); job.destination.clear(); invalid(job); }
TEST(JobPolicy, RejectsZeroRetention) { auto job = validJob(); job.retention = 0; invalid(job); }
TEST(JobPolicy, RejectsInvalidCron) { auto job = validJob(); job.cron = "61 * * * *"; invalid(job); }
TEST(JobPolicy, IgnoresCronWhenNotScheduled) { auto job = validJob(); job.scheduled = false; job.cron = "invalid"; QString error; EXPECT_TRUE(job_policy::validate(job, error)); }
TEST(JobPolicy, RejectsContradictoryFilter) { auto job = validJob(); job.filter.minSize = 20; job.filter.maxSize = 10; invalid(job); }
TEST(JobPolicy, RejectsInvalidFormat) { auto job = validJob(); job.format = static_cast<PackFormat>(2); invalid(job); }
TEST(JobPolicy, RejectsInvalidCompression) { auto job = validJob(); job.compression = static_cast<Compression>(4); invalid(job); }
TEST(JobPolicy, RejectsRealtimeFullBackup) { auto job = validJob(); job.realtime = true; invalid(job); }
TEST(JobPolicy, AllowsRealtimeIncrementalFormatsAndRejectsInvalidModes) {
    auto job = validJob();
    job.tarIncremental = true;
    job.format = PackFormat::Tar;
    job.realtime = true;
    job.scheduled = false;
    job.encrypted = false;
    QString error;
    EXPECT_TRUE(job_policy::validate(job, error)) << error.toStdString();
    job.format = PackFormat::Custom;
    EXPECT_TRUE(job_policy::validate(job, error)) << error.toStdString();
}
TEST(JobPolicy, ValidatesIdentifier) {
    EXPECT_TRUE(job_policy::validId("Abc-123"));
    for (const auto& id : {QString(), QString("../job"), QString("job_name"), QString::fromUtf8("任务")})
        EXPECT_FALSE(job_policy::validId(id));
}
TEST(JobRunPolicy, RejectsUnavailableExecution) {
    using A = job_policy::RunAction;
    EXPECT_EQ(job_policy::request(true, true, true, false, false), A::Reject);
    EXPECT_EQ(job_policy::request(false, false, true, false, false), A::Reject);
    EXPECT_EQ(job_policy::request(false, true, false, false, false), A::Reject);
}
TEST(JobRunPolicy, StartsIdleJob) { EXPECT_EQ(job_policy::request(false, true, true, false, false), job_policy::RunAction::Start); }
TEST(JobRunPolicy, QueuesRunningJob) { EXPECT_EQ(job_policy::request(false, true, true, true, false), job_policy::RunAction::Queue); }
TEST(JobRunPolicy, MergesRepeatedRequest) { EXPECT_EQ(job_policy::request(false, true, true, true, true), job_policy::RunAction::Merge); }
TEST(JobRunPolicy, ExecutesEligiblePendingJob) { EXPECT_TRUE(job_policy::rerun(true, true, true, false)); }
TEST(JobRunPolicy, SuppressesMissingDisabledOrCancelledJobs) {
    EXPECT_FALSE(job_policy::rerun(false, true, true, false));
    EXPECT_FALSE(job_policy::rerun(true, false, true, false));
    EXPECT_FALSE(job_policy::rerun(true, true, false, false));
    EXPECT_FALSE(job_policy::rerun(true, true, true, true));
}
TEST(CronRules, RejectsInvalidFieldCountsAndRanges) {
    CronExpression cron; std::string error;
    for (const auto* value : {"* * * *", "60 * * * *", "* 24 * * *", "* * 0 * *", "* * * 13 *", "*/0 * * * *"})
        EXPECT_FALSE(cron.parse(value, error)) << value;
}
TEST(CronRules, MatchesRangesStepsAndWeekdays) {
    CronExpression cron; std::string error; ASSERT_TRUE(cron.parse("*/15 9-17 * * 1-5", error));
    std::tm t{}; t.tm_min=30; t.tm_hour=10; t.tm_mday=15; t.tm_mon=5; t.tm_wday=1;
    EXPECT_TRUE(cron.matches(t)); t.tm_min=31; EXPECT_FALSE(cron.matches(t));
    t.tm_min=30; t.tm_wday=0; EXPECT_FALSE(cron.matches(t));
}
TEST(CronRules, FindsNextMinuteAndHonorsSearchLimit) {
    CronExpression cron; std::string error; ASSERT_TRUE(cron.parse("*/15 * * * *", error));
    std::tm t{}; t.tm_year=126; t.tm_mon=0; t.tm_mday=5; t.tm_hour=10; t.tm_min=31; t.tm_isdst=-1;
    const auto start=std::mktime(&t); EXPECT_EQ(cron.next(start, 2), static_cast<std::time_t>(-1));
    const auto next=cron.next(start); ASSERT_NE(next, static_cast<std::time_t>(-1));
    std::tm result{}; localtime_r(&next, &result); EXPECT_EQ(result.tm_min, 45);
}

TEST(CronRules, RestrictedDayAndWeekdayUseOrAndSundaySeven) {
    CronExpression cron; std::string error;
    ASSERT_TRUE(cron.parse("0 9 15 * 7", error));
    std::tm t{}; t.tm_min = 0; t.tm_hour = 9; t.tm_mon = 5; t.tm_mday = 15; t.tm_wday = 1;
    EXPECT_TRUE(cron.matches(t));
    t.tm_mday = 16; t.tm_wday = 0; EXPECT_TRUE(cron.matches(t));
    t.tm_wday = 2; EXPECT_FALSE(cron.matches(t));
}
TEST(CronRules, FindsLeapDayBeyondOneYear) {
    CronExpression cron; std::string error; ASSERT_TRUE(cron.parse("0 0 29 2 *", error));
    std::tm t{}; t.tm_year = 125; t.tm_mon = 2; t.tm_mday = 1; t.tm_isdst = -1;
    auto next = cron.next(std::mktime(&t)); ASSERT_NE(next, static_cast<std::time_t>(-1));
    std::tm result{}; localtime_r(&next, &result);
    EXPECT_EQ(result.tm_year, 128); EXPECT_EQ(result.tm_mon, 1); EXPECT_EQ(result.tm_mday, 29);
}

TEST(CronRules, SkipsNonexistentLocalTimeAtSpringForward) {
    const char* previous = std::getenv("TZ");
    const std::string saved = previous ? previous : "";
    const bool hadTimezone = previous != nullptr;
    setenv("TZ", "America/New_York", 1); tzset();
    CronExpression cron; std::string error;
    ASSERT_TRUE(cron.parse("30 2 * * *", error));
    std::tm t{}; t.tm_year = 126; t.tm_mon = 2; t.tm_mday = 8; t.tm_hour = 1; t.tm_min = 59; t.tm_isdst = -1;
    const auto next = cron.next(std::mktime(&t));
    std::tm result{}; localtime_r(&next, &result);
    EXPECT_EQ(result.tm_mday, 9); EXPECT_EQ(result.tm_hour, 2); EXPECT_EQ(result.tm_min, 30);
    if (hadTimezone) setenv("TZ", saved.c_str(), 1); else unsetenv("TZ");
    tzset();
}
TEST(JobPolicy, RejectsScheduleWithoutPossibleDate) {
    auto job = validJob(); job.cron = "0 0 30 2 *"; QString error;
    EXPECT_FALSE(job_policy::validate(job, error));
    EXPECT_FALSE(error.isEmpty());
}

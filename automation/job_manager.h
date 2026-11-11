#pragma once
#include "automation/directory_watcher.h"
#include "core/archive.h"
#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>

struct BackupJob {
    QString id, name, destination, cron;
    QStringList sources;
    FilterOptions filter;
    bool repository = true, encrypted = true;
    PackFormat format = PackFormat::Custom;
    Compression compression = Compression::Deflate;
    bool enabled = true, scheduled = true, realtime = false;
    int retention = 3;
    QDateTime lastRun, lastSuccess, nextRun;
    QString lastResult;
    QString realtimeStatus;
    bool realtimeTriggered = false, fullRescan = false;
    QStringList changedPaths;
};

namespace job_policy {
bool validId(const QString& id);
bool validate(const BackupJob& job, QString& error);
enum class RunAction { Reject, Start, Queue, Merge };
RunAction request(bool paused, bool hasExecutor, bool hasId, bool running, bool pending);
bool rerun(bool pending, bool found, bool enabled, bool cancelled);
} // namespace job_policy

class JobManager : public QObject {
  public:
    using Clock = std::function<QDateTime()>;
    // 加载计划配置并连接定时器；可注入路径以隔离测试数据。
    explicit JobManager(QObject* parent = nullptr, QString configFilePath = {});
    // 返回当前计划列表，只读引用在管理器生命周期内有效。
    const QList<BackupJob>& jobs() const {
        return jobs_;
    }
    // 返回配置读取失败信息；为空表示配置状态正常。
    QString loadError() const {
        return loadError_;
    }
    // 新增或更新计划；校验或原子保存失败时保留原列表。
    bool upsert(BackupJob job, QString& error);
    // 删除计划并清除尚未执行的排队请求。
    bool remove(const QString& id, QString& error);
    // 启用或停用计划，并更新下次运行时间和监控器。
    bool setEnabled(const QString& id, bool enabled, QString& error);
    // 立即运行计划；正在运行时合并为一次后续执行。
    bool runNow(const QString& id, QString& error);
    // 设置具体备份执行器；未设置时计划不会启动。
    void setExecutor(std::function<void(const BackupJob&)> executor) {
        executor_ = std::move(executor);
        flushRebuildRescans();
    }
    // 注入当前时间来源，便于确定性测试计划触发。
    void setClock(Clock clock) {
        clock_ = std::move(clock);
    }
    // 注册计划列表变更回调，用于刷新界面。
    void setOnChanged(std::function<void()> callback) {
        onChanged_ = std::move(callback);
    }
    // 立即检查一次已到期计划。
    void checkSchedules() {
        tick();
    }
    // 记录执行结果并启动符合条件的排队任务。
    void markFinished(const QString& id, bool success, const QString& message, bool partial = false,
                      bool cancelled = false);
    // 启动定时调度并同步实时监控。
    void start();
    // 暂停或恢复自动触发，不修改计划配置。
    void pause(bool paused);

  private:
    // 读取并校验计划配置，失败时保留具体加载错误。
    void load();
    // 原子保存当前计划列表，可返回底层写入错误。
    bool save(QString* error = nullptr);
    // 检查定时到期项并更新下一次执行时间。
    void tick();
    // 根据 cron 表达式更新计划的下次运行时间。
    void updateNext(BackupJob& job);
    // 为启用的实时任务创建、停止或重建目录监控器。
    void syncWatchers(const QString& changedId = {});
    void flushRebuildRescans();
    // 立即运行或合并同一计划的后续请求。
    bool requestRun(const BackupJob& job);
    QList<BackupJob> jobs_;
    QTimer timer_;
    bool paused_ = false;
    std::function<void(const BackupJob&)> executor_;
    std::map<std::string, std::unique_ptr<DirectoryWatcher>> watchers_;
    std::map<std::string, int> realtimeGeneration_;
    std::map<std::string, QDateTime> realtimeFirst_;
    std::map<std::string, int> watcherGeneration_;
    QMap<QString, QMap<QString, QString>> watcherStatuses_;
    QSet<QString> running_;
    QSet<QString> pending_;
    QMap<QString, BackupJob> pendingJobs_;
    QMap<QString, QSet<QString>> realtimeChanges_;
    QSet<QString> realtimeRescans_;
    QSet<QString> rebuildRescans_;
    Clock clock_;
    std::function<void()> onChanged_;
    QString configFilePath_;
    QString loadError_;
};

#pragma once
#include <QObject>
#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <QSet>
#include <QMap>
#include <functional>
#include <map>
#include <memory>
#include "automation/directory_watcher.h"
#include "core/archive.h"

struct BackupJob {
    QString id, name, destination, cron;
    QStringList sources;
    FilterOptions filter;
    bool repository=true, encrypted=true;
    PackFormat format=PackFormat::Custom;
    Compression compression=Compression::Deflate;
    bool enabled=true, scheduled=true, realtime=false;
    int retention=3;
    QDateTime lastRun, nextRun;
    QString lastResult;
    QString realtimeStatus;
    bool realtimeTriggered=false, fullRescan=false;
    QStringList changedPaths;
};

class JobManager : public QObject {
 public:
    using Clock = std::function<QDateTime()>;
    explicit JobManager(QObject *parent=nullptr, QString configFilePath={});
    const QList<BackupJob> &jobs() const { return jobs_; }
    QString loadError() const { return loadError_; }
    bool upsert(BackupJob job, QString &error);
    bool remove(const QString &id, QString &error);
    bool setEnabled(const QString &id, bool enabled, QString &error);
    bool runNow(const QString &id, QString &error);
    void setExecutor(std::function<void(const BackupJob &)> executor) { executor_=std::move(executor); }
    void setClock(Clock clock) { clock_=std::move(clock); }
    void setOnChanged(std::function<void()> callback) { onChanged_=std::move(callback); }
    void checkSchedules() { tick(); }
    void markFinished(const QString &id, bool success, const QString &message, bool partial = false, bool cancelled = false);
    void start();
    void pause(bool paused);
 private:
    void load(); bool save(QString *error=nullptr); void tick(); void updateNext(BackupJob &job); void syncWatchers();
    bool requestRun(const BackupJob &job);
    QList<BackupJob> jobs_; QTimer timer_; bool paused_=false;
    std::function<void(const BackupJob &)> executor_;
    std::map<std::string,std::unique_ptr<DirectoryWatcher>> watchers_;
    std::map<std::string,int> realtimeGeneration_;
    std::map<std::string,QDateTime> realtimeFirst_;
    std::map<std::string,int> watcherGeneration_;
    QMap<QString,QMap<QString,QString>> watcherStatuses_;
    QSet<QString> running_;
    QSet<QString> pending_;
    QMap<QString, BackupJob> pendingJobs_;
    QMap<QString, QSet<QString>> realtimeChanges_;
    QSet<QString> realtimeRescans_;
    Clock clock_;
    std::function<void()> onChanged_;
    QString configFilePath_;
    QString loadError_;
};

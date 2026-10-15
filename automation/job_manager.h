#pragma once
#include <QObject>
#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <QSet>
#include <functional>
#include <map>
#include <memory>
#include "automation/directory_watcher.h"
#include "archive/archive.h"

struct BackupJob {
    QString id, name, destination, cron;
    QStringList sources;
    QString repositoryType = "local";
    QString backupMode = "incremental";
    QString remoteProfile;
    int compressionLevel = 6;
    FilterOptions filter;
    bool enabled=true, realtime=false;
    int retention=3;
    QDateTime lastRun, nextRun;
    QString lastResult;
    QString realtimeStatus;
};

class JobManager : public QObject {
 public:
    using Clock = std::function<QDateTime()>;
    explicit JobManager(QObject *parent=nullptr, QString configFilePath={});
    const QList<BackupJob> &jobs() const { return jobs_; }
    bool upsert(BackupJob job, QString &error);
    bool remove(const QString &id, QString &error);
    bool setEnabled(const QString &id, bool enabled, QString &error);
    bool runNow(const QString &id, QString &error);
    void setExecutor(std::function<void(const BackupJob &)> executor) { executor_=std::move(executor); }
    void setClock(Clock clock) { clock_=std::move(clock); }
    void checkSchedules() { tick(); }
    void markFinished(const QString &id, bool success, const QString &message);
    void start();
    void pause(bool paused);
 private:
    void load(); void save(); void tick(); void updateNext(BackupJob &job); void syncWatchers();
    bool requestRun(const BackupJob &job);
    QList<BackupJob> jobs_; QTimer timer_; bool paused_=false;
    std::function<void(const BackupJob &)> executor_;
    std::map<std::string,std::unique_ptr<DirectoryWatcher>> watchers_;
    std::map<std::string,int> realtimeGeneration_;
    std::map<std::string,QDateTime> realtimeFirst_;
    std::map<std::string,int> watcherGeneration_;
    QSet<QString> running_;
    QSet<QString> pending_;
    Clock clock_;
    QString configFilePath_;
};

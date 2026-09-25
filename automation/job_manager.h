#pragma once
#include <QObject>
#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include "automation/directory_watcher.h"

struct BackupJob {
    QString id, name, destination, cron;
    QStringList sources;
    bool enabled=true, realtime=false;
    int retention=3;
    QDateTime lastRun, nextRun;
    QString lastResult;
};

class JobManager : public QObject {
 public:
    explicit JobManager(QObject *parent=nullptr);
    const QList<BackupJob> &jobs() const { return jobs_; }
    bool upsert(BackupJob job, QString &error);
    bool remove(const QString &id, QString &error);
    void setExecutor(std::function<void(const BackupJob &)> executor) { executor_=std::move(executor); }
    void markFinished(const QString &id, bool success, const QString &message);
    void start();
    void pause(bool paused);
 private:
    void load(); void save(); void tick(); void updateNext(BackupJob &job); void syncWatchers();
    QList<BackupJob> jobs_; QTimer timer_; bool paused_=false;
    std::function<void(const BackupJob &)> executor_;
    std::map<std::string,std::unique_ptr<DirectoryWatcher>> watchers_;
    std::map<std::string,int> realtimeGeneration_;
    std::map<std::string,QDateTime> realtimeFirst_;
};

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>

class RemoteClient;

class RemoteRepositoryService final : public QObject {
public:
    using Completion = std::function<void(bool, QString)>;
    using Progress = std::function<void(int, QString)>;
    explicit RemoteRepositoryService(RemoteClient *client, QObject *parent=nullptr);
    void uploadSnapshot(const QString &repository, const QString &snapshotId,
                        std::atomic_bool *cancelled, Progress progress, Completion done);
    void downloadSnapshot(const QString &snapshotId, const QString &repository,
                          std::atomic_bool *cancelled, Progress progress, Completion done);
private:
    RemoteClient *client_;
};

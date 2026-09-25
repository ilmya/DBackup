#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <functional>

class RemoteClient : public QObject {
 public:
    explicit RemoteClient(QObject *parent=nullptr);
    void configure(QUrl endpoint, QString credentialName);
    void registerUser(const QString &user,const QString &password,std::function<void(bool,QString)> done);
    void login(const QString &user,const QString &password,std::function<void(bool,QString)> done);
    void checkChunks(const QStringList &hashes,std::function<void(QStringList,QString)> done);
    void uploadChunk(const QString &hash,const QByteArray &data,std::function<void(bool,QString)> done);
    void downloadChunk(const QString &hash,std::function<void(QByteArray,QString)> done);
    void commitSnapshot(const QJsonObject &manifest,std::function<void(bool,QString)> done);
    void deleteSnapshot(const QString &id,std::function<void(bool,QString)> done);
    void listSnapshots(std::function<void(QJsonArray,QString)> done);
 private:
    QNetworkRequest request(const QString &path) const;
    void postCredentials(const QString &path,const QString &user,const QString &password,std::function<void(bool,QString)> done,bool saveToken);
    QUrl endpoint_;QString credentialName_;QByteArray token_;QNetworkAccessManager network_;
};

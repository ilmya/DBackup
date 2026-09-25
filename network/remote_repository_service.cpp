#include "network/remote_repository_service.h"
#include "network/remote_client.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QSharedPointer>

RemoteRepositoryService::RemoteRepositoryService(RemoteClient *client,QObject *parent):QObject(parent),client_(client){}

void RemoteRepositoryService::uploadSnapshot(const QString&root,const QString&id,std::atomic_bool*cancelled,Progress progress,Completion done){
    QFile config(root+"/config.bin"),manifest(root+"/snapshots/"+id+".manifest");
    if(!config.open(QIODevice::ReadOnly)||!manifest.open(QIODevice::ReadOnly)){done(false,tr("仓库配置或快照清单不存在"));return;}
    QJsonObject remote{{"id",id},{"name",id},{"config",QString::fromLatin1(config.readAll().toBase64())},{"manifest",QString::fromLatin1(manifest.readAll().toBase64())}};
    QStringList hashes,paths;QDirIterator it(root+"/chunks",{"*.blk"},QDir::Files,QDirIterator::Subdirectories);while(it.hasNext()){const QString path=it.next(),hash=QFileInfo(path).completeBaseName();if(hash.size()==64){hashes<<hash;paths<<path;}}
    QJsonArray all;for(const auto&hash:hashes)all.append(hash);remote["chunks"]=all;
    client_->checkChunks(hashes,[=](QStringList missing,QString error){if(!error.isEmpty()){done(false,error);return;}auto index=QSharedPointer<int>::create(0);auto uploadNext=QSharedPointer<std::function<void()>>::create();*uploadNext=[=]{if(cancelled&&cancelled->load()){done(false,tr("操作已取消"));return;}if(*index>=missing.size()){client_->commitSnapshot(remote,[=](bool ok,QString commitError){done(ok,commitError);});return;}const QString hash=missing[*index],path=paths.value(hashes.indexOf(hash));QFile file(path);if(!file.open(QIODevice::ReadOnly)){done(false,tr("无法读取数据块：%1").arg(hash));return;}client_->uploadChunk(hash,file.readAll(),[=](bool ok,QString uploadError){if(!ok){done(false,uploadError);return;}++*index;if(progress)progress(missing.isEmpty()?100:(100**index/missing.size()),hash);(*uploadNext)();});};(*uploadNext)();});
}

void RemoteRepositoryService::downloadSnapshot(const QString&id,const QString&root,std::atomic_bool*cancelled,Progress progress,Completion done){
    client_->getSnapshot(id,[=](QJsonObject remote,QString error){if(!error.isEmpty()){done(false,error);return;}const QByteArray config=QByteArray::fromBase64(remote["config"].toString().toLatin1()),manifest=QByteArray::fromBase64(remote["manifest"].toString().toLatin1());if(config.isEmpty()||manifest.isEmpty()){done(false,tr("远程快照清单无效"));return;}QDir().mkpath(root+"/snapshots");QDir().mkpath(root+"/chunks");QSaveFile configFile(root+"/config.bin");if(!configFile.open(QIODevice::WriteOnly)||configFile.write(config)!=config.size()||!configFile.commit()){done(false,tr("无法写入仓库配置"));return;}QStringList hashes;for(auto value:remote["chunks"].toArray())hashes<<value.toString();auto index=QSharedPointer<int>::create(0);auto downloadNext=QSharedPointer<std::function<void()>>::create();*downloadNext=[=]{if(cancelled&&cancelled->load()){done(false,tr("操作已取消"));return;}if(*index>=hashes.size()){QSaveFile output(root+"/snapshots/"+id+".manifest");if(!output.open(QIODevice::WriteOnly)||output.write(manifest)!=manifest.size()||!output.commit()){done(false,tr("无法提交快照清单"));return;}done(true,{});return;}const QString hash=hashes[*index],dir=root+"/chunks/"+hash.left(2),path=dir+"/"+hash+".blk";if(QFileInfo::exists(path)){++*index;(*downloadNext)();return;}client_->downloadChunk(hash,[=](QByteArray data,QString downloadError){if(!downloadError.isEmpty()){done(false,downloadError);return;}QDir().mkpath(dir);QSaveFile output(path);if(!output.open(QIODevice::WriteOnly)||output.write(data)!=data.size()||!output.commit()){done(false,tr("无法保存数据块：%1").arg(hash));return;}++*index;if(progress)progress(hashes.isEmpty()?100:(100**index/hashes.size()),hash);(*downloadNext)();});};(*downloadNext)();});
}

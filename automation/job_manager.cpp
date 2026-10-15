#include "automation/job_manager.h"
#include "automation/cron.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QUuid>

namespace { QString filePath(){QString dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);QDir().mkpath(dir);return dir+"/jobs.json";} }
JobManager::JobManager(QObject*p,QString configFilePath):QObject(p),configFilePath_(configFilePath.isEmpty()?filePath():std::move(configFilePath)){connect(&timer_,&QTimer::timeout,this,[this]{tick();});load();}
void JobManager::start(){timer_.start(30000);syncWatchers();tick();}void JobManager::pause(bool value){paused_=value;}
bool JobManager::upsert(BackupJob job,QString&error){CronExpression expression;std::string detail;if(!expression.parse(job.cron.toStdString(),detail)){error=QString::fromStdString(detail);return false;}if(job.sources.isEmpty()||job.destination.isEmpty()){error=tr("来源和仓库不能为空");return false;}if(job.id.isEmpty())job.id=QUuid::createUuid().toString(QUuid::WithoutBraces);updateNext(job);for(auto&existing:jobs_)if(existing.id==job.id){existing=job;save();syncWatchers();return true;}jobs_.append(job);save();syncWatchers();return true;}
bool JobManager::remove(const QString&id,QString&error){for(int i=0;i<jobs_.size();++i)if(jobs_[i].id==id){const std::string jobPrefix=(id+"|").toUtf8().toStdString();++watcherGeneration_[id.toUtf8().toStdString()];jobs_.removeAt(i);pending_.remove(id);for(auto it=watchers_.begin();it!=watchers_.end();)if(it->first.rfind(jobPrefix,0)==0)it=watchers_.erase(it);else++it;for(auto it=realtimeGeneration_.begin();it!=realtimeGeneration_.end();)if(it->first.rfind(jobPrefix,0)==0)it=realtimeGeneration_.erase(it);else++it;for(auto it=realtimeFirst_.begin();it!=realtimeFirst_.end();)if(it->first.rfind(jobPrefix,0)==0)it=realtimeFirst_.erase(it);else++it;save();syncWatchers();return true;}error=tr("任务不存在");return false;}
bool JobManager::setEnabled(const QString&id,bool enabled,QString&error){for(auto&job:jobs_)if(job.id==id){if(job.enabled==enabled)return true;job.enabled=enabled;if(enabled)updateNext(job);else{job.nextRun={};pending_.remove(id);}syncWatchers();save();return true;}error=tr("任务不存在");return false;}
bool JobManager::runNow(const QString&id,QString&error){for(const auto&job:jobs_)if(job.id==id){if(paused_){error=tr("自动任务已暂停，无法立即运行");return false;}if(!executor_){error=tr("备份执行器尚未就绪");return false;}const bool alreadyRunning=running_.contains(id);if(!requestRun(job)){error=tr("任务未能加入运行队列");return false;}if(alreadyRunning)error=tr("任务正在运行，已排队一次");return true;}error=tr("任务不存在");return false;}
void JobManager::updateNext(BackupJob&job){CronExpression c;std::string e;if(c.parse(job.cron.toStdString(),e)){const QDateTime now=clock_?clock_():QDateTime::currentDateTime();const auto next=c.next(now.toSecsSinceEpoch());if(next!=-1)job.nextRun=QDateTime::fromSecsSinceEpoch(next);}}
bool JobManager::requestRun(const BackupJob&job){if(paused_||!executor_||job.id.isEmpty())return false;if(running_.contains(job.id)){pending_.insert(job.id);return true;}running_.insert(job.id);executor_(job);return true;}
void JobManager::tick(){if(paused_||!executor_)return;const QDateTime now=clock_?clock_():QDateTime::currentDateTime();bool changed=false;for(auto&job:jobs_)if(job.enabled&&job.nextRun.isValid()&&job.nextRun<=now){requestRun(job);updateNext(job);changed=true;}if(changed)save();}
void JobManager::syncWatchers() {
    watchers_.clear();
    realtimeGeneration_.clear();
    realtimeFirst_.clear();

    for (auto &job : jobs_) {
        const std::string jobId = job.id.toUtf8().toStdString();
        const int watcherEpoch = ++watcherGeneration_[jobId];
        if (!job.enabled || !job.realtime) {
            job.realtimeStatus = job.realtime ? tr("监控已暂停") : tr("未启用实时备份");
            continue;
        }

        job.realtimeStatus = tr("正在连接监控目录");
        const QString destination = QDir::cleanPath(job.destination);
        for (const QString &source : job.sources) {
            const std::string sourceUtf8 = source.toUtf8().toStdString();
            const std::string key = jobId + "|" + sourceUtf8;
            const QString sourceRoot = QDir::cleanPath(source);
            QString excluded;
            if (destination.startsWith(sourceRoot + "/", Qt::CaseInsensitive) ||
                destination.startsWith(sourceRoot + "\\", Qt::CaseInsensitive)) {
                excluded = QDir(sourceRoot).relativeFilePath(destination).replace('\\', '/');
            }

            auto watcher = std::make_unique<DirectoryWatcher>();
            std::string error;
            const bool started = watcher->start(sourceUtf8,
                [this, id = job.id, key, excluded, watcherEpoch]
                (const std::vector<std::string> &paths, bool fullRescan) {
                    if (!excluded.isEmpty() && !paths.empty() && !fullRescan) {
                        bool onlyOutput = true;
                        for (const auto &path : paths) {
                            const QString changed = QString::fromUtf8(path.c_str()).replace('\\', '/');
                            if (!changed.startsWith(excluded + "/") && changed != excluded) {
                                onlyOutput = false;
                                break;
                            }
                        }
                        if (onlyOutput) return;
                    }

                    QMetaObject::invokeMethod(this, [this, id, key, watcherEpoch] {
                        const auto currentEpoch = watcherGeneration_.find(id.toUtf8().toStdString());
                        if (currentEpoch == watcherGeneration_.end() || currentEpoch->second != watcherEpoch) return;

                        const int generation = ++realtimeGeneration_[key];
                        if (!realtimeFirst_[key].isValid()) {
                            realtimeFirst_[key] = QDateTime::currentDateTime();
                            const QDateTime first = realtimeFirst_[key];
                            QTimer::singleShot(30000, this, [this, id, key, first, watcherEpoch] {
                                const auto epoch = watcherGeneration_.find(id.toUtf8().toStdString());
                                if (epoch == watcherGeneration_.end() || epoch->second != watcherEpoch ||
                                    paused_ || realtimeFirst_[key] != first || !executor_) return;
                                realtimeFirst_[key] = {};
                                ++realtimeGeneration_[key];
                                for (const auto &candidate : jobs_) {
                                    if (candidate.id == id && candidate.enabled) {
                                        requestRun(candidate);
                                        break;
                                    }
                                }
                            });
                        }
                        QTimer::singleShot(3000, this, [this, id, key, generation, watcherEpoch] {
                            const auto epoch = watcherGeneration_.find(id.toUtf8().toStdString());
                            if (epoch == watcherGeneration_.end() || epoch->second != watcherEpoch ||
                                paused_ || realtimeGeneration_[key] != generation || !executor_) return;
                            realtimeFirst_[key] = {};
                            for (const auto &candidate : jobs_) {
                                if (candidate.id == id && candidate.enabled) {
                                    requestRun(candidate);
                                    break;
                                }
                            }
                        });
                    }, Qt::QueuedConnection);
                }, error, 0, 0,
                [this, id = job.id, watcherEpoch](bool available, const std::string &message) {
                    const QString status = available ? tr("监控中") : QString::fromUtf8(message.c_str());
                    QMetaObject::invokeMethod(this, [this, id, watcherEpoch, status] {
                        const auto epoch = watcherGeneration_.find(id.toUtf8().toStdString());
                        if (epoch == watcherGeneration_.end() || epoch->second != watcherEpoch) return;
                        for (auto &candidate : jobs_) {
                            if (candidate.id == id) {
                                candidate.realtimeStatus = status;
                                break;
                            }
                        }
                    }, Qt::QueuedConnection);
                });

            if (started && watcher->running()) watchers_[key] = std::move(watcher);
            else job.realtimeStatus = QString::fromUtf8(error.c_str());
        }
    }
}
void JobManager::markFinished(const QString&id,bool success,const QString&message){running_.remove(id);BackupJob rerun;bool found=false;const bool shouldRerun=pending_.remove(id)>0;for(auto&job:jobs_)if(job.id==id){job.lastRun=clock_?clock_():QDateTime::currentDateTime();job.lastResult=(success?tr("成功："):tr("失败："))+message;if(job.enabled)updateNext(job);else job.nextRun={};rerun=job;found=true;break;}if(found)save();if(shouldRerun&&found&&rerun.enabled)requestRun(rerun);}
void JobManager::save(){QJsonArray array;for(const auto&j:jobs_){QJsonObject filter{{"pathContains",QString::fromUtf8(j.filter.pathContains.c_str())},{"nameContains",QString::fromUtf8(j.filter.nameContains.c_str())},{"extension",QString::fromUtf8(j.filter.extension.c_str())},{"ownerContains",QString::fromUtf8(j.filter.ownerContains.c_str())},{"type",static_cast<int>(j.filter.type)},{"minSize",static_cast<double>(j.filter.minSize)},{"maxSize",static_cast<double>(j.filter.maxSize)},{"afterMtimeMs",static_cast<double>(j.filter.afterMtimeMs)},{"beforeMtimeMs",static_cast<double>(j.filter.beforeMtimeMs)}};QJsonArray extensions;for(const auto&s:j.filter.extensions)extensions.append(QString::fromUtf8(s.c_str()));filter["extensions"]=extensions;QJsonArray rules;for(const auto&r:j.filter.pathRules)rules.append(QJsonObject{{"include",r.include},{"pattern",QString::fromUtf8(r.pattern.c_str())}});filter["pathRules"]=rules;QJsonObject o{{"id",j.id},{"name",j.name},{"destination",j.destination},{"cron",j.cron},{"repositoryType",j.repositoryType},{"backupMode",j.backupMode},{"remoteProfile",j.remoteProfile},{"compressionLevel",j.compressionLevel},{"filter",filter},{"enabled",j.enabled},{"realtime",j.realtime},{"retention",j.retention},{"lastRun",j.lastRun.toString(Qt::ISODateWithMs)},{"nextRun",j.nextRun.toString(Qt::ISODateWithMs)},{"lastResult",j.lastResult}};QJsonArray sources;for(auto&s:j.sources)sources.append(s);o["sources"]=sources;array.append(o);}QSaveFile f(configFilePath_);if(!f.open(QIODevice::WriteOnly)){qWarning("JobManager config save failed: %s (%s)",qPrintable(f.fileName()),qPrintable(f.errorString()));return;}const QByteArray payload=QJsonDocument(QJsonObject{{"version",2},{"jobs",array}}).toJson();const qint64 written=f.write(payload);if(written!=payload.size()){qWarning("JobManager config save failed: %s (%s)",qPrintable(f.fileName()),qPrintable(f.errorString()));f.cancelWriting();return;}if(!f.commit())qWarning("JobManager config save failed: %s (%s)",qPrintable(f.fileName()),qPrintable(f.errorString()));}
void JobManager::load(){
    QFile file(configFilePath_);
    if(!file.open(QIODevice::ReadOnly))return;
    const QJsonDocument document=QJsonDocument::fromJson(file.readAll());
    if(!document.isObject())return;
    const QJsonObject root=document.object();
    if(root.value("version").toInt(-1)!=2||!root.value("jobs").isArray())return;

    const auto hasKeys=[](const QJsonObject&object,const QStringList&keys){
        for(const QString&key:keys)if(!object.contains(key))return false;
        return true;
    };
    const QStringList jobKeys={"id","name","destination","cron","repositoryType","backupMode",
        "remoteProfile","compressionLevel","filter","enabled","realtime","retention",
        "lastRun","nextRun","lastResult","sources"};
    const QStringList filterKeys={"pathContains","nameContains","extension","ownerContains","type",
        "minSize","maxSize","afterMtimeMs","beforeMtimeMs","extensions","pathRules"};

    QList<BackupJob> loaded;
    for(const QJsonValue&value:root.value("jobs").toArray()){
        if(!value.isObject())return;
        const QJsonObject object=value.toObject();
        if(!hasKeys(object,jobKeys)||!object.value("filter").isObject()||!object.value("sources").isArray())return;
        const QJsonObject filter=object.value("filter").toObject();
        if(!hasKeys(filter,filterKeys)||!filter.value("extensions").isArray()||!filter.value("pathRules").isArray())return;

        BackupJob job;
        job.id=object.value("id").toString();
        job.name=object.value("name").toString();
        job.destination=object.value("destination").toString();
        job.cron=object.value("cron").toString();
        job.repositoryType=object.value("repositoryType").toString();
        job.backupMode=object.value("backupMode").toString();
        job.remoteProfile=object.value("remoteProfile").toString();
        job.compressionLevel=object.value("compressionLevel").toInt();
        job.filter.pathContains=filter.value("pathContains").toString().toUtf8().toStdString();
        job.filter.nameContains=filter.value("nameContains").toString().toUtf8().toStdString();
        job.filter.extension=filter.value("extension").toString().toUtf8().toStdString();
        job.filter.ownerContains=filter.value("ownerContains").toString().toUtf8().toStdString();
        job.filter.type=static_cast<EntryTypeFilter>(filter.value("type").toInt());
        job.filter.minSize=static_cast<uint64_t>(filter.value("minSize").toDouble());
        job.filter.maxSize=static_cast<uint64_t>(filter.value("maxSize").toDouble());
        job.filter.afterMtimeMs=static_cast<int64_t>(filter.value("afterMtimeMs").toDouble());
        job.filter.beforeMtimeMs=static_cast<int64_t>(filter.value("beforeMtimeMs").toDouble());
        for(const QJsonValue&extension:filter.value("extensions").toArray()){
            if(!extension.isString())return;
            job.filter.extensions.push_back(extension.toString().toUtf8().toStdString());
        }
        for(const QJsonValue&ruleValue:filter.value("pathRules").toArray()){
            if(!ruleValue.isObject())return;
            const QJsonObject rule=ruleValue.toObject();
            if(!rule.value("include").isBool()||!rule.value("pattern").isString())return;
            job.filter.pathRules.push_back({rule.value("include").toBool(),rule.value("pattern").toString().toUtf8().toStdString()});
        }
        job.enabled=object.value("enabled").toBool();
        job.realtime=object.value("realtime").toBool();
        job.retention=object.value("retention").toInt();
        job.lastRun=QDateTime::fromString(object.value("lastRun").toString(),Qt::ISODateWithMs);
        job.lastResult=object.value("lastResult").toString();
        for(const QJsonValue&source:object.value("sources").toArray()){
            if(!source.isString())return;
            job.sources<<source.toString();
        }
        updateNext(job);
        loaded<<job;
    }
    jobs_=std::move(loaded);
}

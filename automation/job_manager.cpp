#include "automation/job_manager.h"
#include "automation/cron.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QRegularExpression>

namespace { QString filePath(){QString dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);QDir().mkpath(dir);return dir+"/jobs.json";} }
JobManager::JobManager(QObject*p,QString configFilePath):QObject(p),configFilePath_(configFilePath.isEmpty()?filePath():std::move(configFilePath)){connect(&timer_,&QTimer::timeout,this,[this]{tick();});load();}
void JobManager::start(){timer_.start(30000);syncWatchers();tick();}void JobManager::pause(bool value){paused_=value;}
bool JobManager::upsert(BackupJob job,QString&error){
    if (!loadError_.isEmpty()) { error=loadError_; return false; }
    CronExpression expression; std::string detail;
    if (!job.repository && job.realtime) { error=tr("独立归档不支持实时备份"); return false; }
    if (job.repository && !job.encrypted) { error=tr("版本仓库需要加密密码"); return false; }
    if (job.scheduled && !expression.parse(job.cron.toStdString(),detail)) { error=QString::fromStdString(detail); return false; }
    if (job.name.trimmed().isEmpty() || job.sources.isEmpty() || job.destination.isEmpty() || job.retention<1) { error=tr("计划名称、来源、目标和保留数量无效"); return false; }
    if (!ValidateFilterOptions(job.filter, detail)) { error=QString::fromStdString(detail); return false; }
    if (static_cast<int>(job.format)>1 || static_cast<int>(job.compression)>3) { error=tr("无效的归档格式或压缩算法"); return false; }
    if (job.id.isEmpty()) job.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9-]+$")).match(job.id).hasMatch()) { error=tr("计划标识无效"); return false; }
    updateNext(job); const auto before=jobs_; bool replaced=false;
    for(auto&existing:jobs_) if(existing.id==job.id){existing=job;replaced=true;break;}
    if(!replaced) jobs_.append(job);
    if(!save(&error)){jobs_=before;return false;}
    syncWatchers(); if(onChanged_)onChanged_(); return true;
}
bool JobManager::remove(const QString&id,QString&error){
    const auto before=jobs_; int index=-1; for(int i=0;i<jobs_.size();++i)if(jobs_[i].id==id){index=i;break;}
    if(index<0){error=tr("任务不存在");return false;}
    jobs_.removeAt(index); if(!save(&error)){jobs_=before;return false;}
    pending_.remove(id); syncWatchers(); if(onChanged_)onChanged_(); return true;
}
bool JobManager::setEnabled(const QString&id,bool enabled,QString&error){
    for(auto&job:jobs_)if(job.id==id){BackupJob changed=job;changed.enabled=enabled;if(!upsert(changed,error))return false;if(!enabled)pending_.remove(id);return true;}
    error=tr("任务不存在");return false;
}
bool JobManager::runNow(const QString&id,QString&error){for(const auto&job:jobs_)if(job.id==id){if(paused_){error=tr("自动任务已暂停，无法立即运行");return false;}if(!executor_){error=tr("备份执行器尚未就绪");return false;}const bool alreadyRunning=running_.contains(id);if(!requestRun(job)){error=tr("任务未能加入运行队列");return false;}if(alreadyRunning)error=tr("任务正在运行，已排队一次");return true;}error=tr("任务不存在");return false;}
void JobManager::updateNext(BackupJob&job){if(!job.enabled||!job.scheduled){job.nextRun={};return;}CronExpression c;std::string e;if(c.parse(job.cron.toStdString(),e)){const QDateTime now=clock_?clock_():QDateTime::currentDateTime();const auto next=c.next(now.toSecsSinceEpoch());if(next!=-1)job.nextRun=QDateTime::fromSecsSinceEpoch(next);}}
bool JobManager::requestRun(const BackupJob&job){if(paused_||!executor_||job.id.isEmpty())return false;if(running_.contains(job.id)){pending_.insert(job.id);return true;}running_.insert(job.id);executor_(job);return true;}
void JobManager::tick(){if(paused_||!executor_)return;const QDateTime now=clock_?clock_():QDateTime::currentDateTime();bool changed=false;for(auto&job:jobs_)if(job.enabled&&job.nextRun.isValid()&&job.nextRun<=now){requestRun(job);updateNext(job);changed=true;}if(changed)save();}
void JobManager::syncWatchers() {
    watchers_.clear();
    realtimeGeneration_.clear();
    realtimeFirst_.clear();

    for (auto &job : jobs_) {
        const std::string jobId = job.id.toUtf8().toStdString();
        const int watcherEpoch = ++watcherGeneration_[jobId];
        watcherStatuses_.remove(job.id);
        if (!job.enabled || !job.realtime) {
            job.realtimeStatus = job.realtime ? tr("监控已暂停") : tr("未启用实时备份");
            continue;
        }

        job.realtimeStatus = tr("正在连接监控目录");
        const QString destination = QDir::cleanPath(job.destination);
        for (const QString &source : job.sources) {
            const QString selectedPath = QDir::cleanPath(source);
            const QFileInfo selectedInfo(selectedPath);
            const bool directory = selectedInfo.isDir() && !selectedInfo.isSymLink();
            const QString sourceRoot = directory ? selectedPath : selectedInfo.absolutePath();
            const QString selectedFile = directory ? QString() : selectedInfo.fileName();
            const std::string sourceUtf8 = sourceRoot.toUtf8().toStdString();
            const std::string key = jobId + "|" + selectedPath.toUtf8().toStdString();
            QString excluded;
            if (destination.startsWith(sourceRoot + "/", Qt::CaseSensitive)) {
                excluded = QDir(sourceRoot).relativeFilePath(destination);
            }

            auto watcher = std::make_unique<DirectoryWatcher>();
            std::string error;
            const bool started = watcher->start(sourceUtf8,
                [this, id = job.id, key, excluded, selectedFile, watcherEpoch]
                (const std::vector<std::string> &paths, bool fullRescan) {
                    if (!selectedFile.isEmpty() && !fullRescan) {
                        bool relevant = false;
                        for (const auto &path : paths) if (QString::fromUtf8(path.c_str()) == selectedFile) { relevant = true; break; }
                        if (!relevant) return;
                    }
                    if (!excluded.isEmpty() && !paths.empty() && !fullRescan) {
                        bool onlyOutput = true;
                        for (const auto &path : paths) {
                            const QString changed = QString::fromUtf8(path.c_str());
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
                [this, id = job.id, selectedPath, watcherEpoch](bool available, const std::string &message) {
                    const QString status = available && message.find("部分目录无法监控") == std::string::npos ? tr("监控中") : QString::fromUtf8(message.c_str());
                    QMetaObject::invokeMethod(this, [this, id, selectedPath, watcherEpoch, status] {
                        const auto epoch = watcherGeneration_.find(id.toUtf8().toStdString());
                        if (epoch == watcherGeneration_.end() || epoch->second != watcherEpoch) return;
                        watcherStatuses_[id][selectedPath]=status;
                        QString aggregate=tr("监控中");
                        for (const auto &value:watcherStatuses_[id]) if(value!=tr("监控中")){aggregate=value;break;}
                        for (auto &candidate : jobs_) {
                            if (candidate.id == id) {
                                candidate.realtimeStatus = aggregate;
                                break;
                            }
                        }
                        if (onChanged_) onChanged_();
                    }, Qt::QueuedConnection);
                }, directory);

            if (started && watcher->running()) watchers_[key] = std::move(watcher);
            else { job.realtimeStatus = QString::fromUtf8(error.c_str()); watcherStatuses_[job.id][selectedPath]=job.realtimeStatus; }
        }
    }
}
void JobManager::markFinished(const QString&id,bool success,const QString&message,bool partial,bool cancelled){running_.remove(id);BackupJob rerun;bool found=false;const bool shouldRerun=pending_.remove(id)>0;for(auto&job:jobs_)if(job.id==id){job.lastRun=clock_?clock_():QDateTime::currentDateTime();job.lastResult=(cancelled?tr("已取消："):(success?(partial?tr("部分完成："):tr("成功：")):tr("失败：")))+message;if(job.enabled)updateNext(job);else job.nextRun={};rerun=job;found=true;break;}if(found){save();if(onChanged_)onChanged_();}if(shouldRerun&&found&&rerun.enabled&&!cancelled)requestRun(rerun);}
bool JobManager::save(QString *saveError){QJsonArray array;for(const auto&j:jobs_){QJsonObject filter{{"pathContains",QString::fromUtf8(j.filter.pathContains.c_str())},{"nameContains",QString::fromUtf8(j.filter.nameContains.c_str())},{"extension",QString::fromUtf8(j.filter.extension.c_str())},{"ownerContains",QString::fromUtf8(j.filter.ownerContains.c_str())},{"type",static_cast<int>(j.filter.type)},{"minSize",static_cast<double>(j.filter.minSize)},{"maxSize",static_cast<double>(j.filter.maxSize)},{"afterMtimeMs",static_cast<double>(j.filter.afterMtimeMs)},{"beforeMtimeMs",static_cast<double>(j.filter.beforeMtimeMs)}};QJsonArray extensions;for(const auto&s:j.filter.extensions)extensions.append(QString::fromUtf8(s.c_str()));filter["extensions"]=extensions;QJsonArray rules;for(const auto&r:j.filter.pathRules)rules.append(QJsonObject{{"include",r.include},{"pattern",QString::fromUtf8(r.pattern.c_str())}});filter["pathRules"]=rules;QJsonArray selected;for(const auto&r:j.filter.sourceRules)selected.append(QJsonObject{{"include",r.include},{"path",QString::fromUtf8(r.path.c_str())}});filter["sourceRules"]=selected;QJsonObject o{{"repository",j.repository},{"encrypted",j.encrypted},{"format",static_cast<int>(j.format)},{"compression",static_cast<int>(j.compression)},{"id",j.id},{"name",j.name},{"destination",j.destination},{"cron",j.cron},{"filter",filter},{"enabled",j.enabled},{"scheduled",j.scheduled},{"realtime",j.realtime},{"retention",j.retention},{"lastRun",j.lastRun.toString(Qt::ISODateWithMs)},{"nextRun",j.nextRun.toString(Qt::ISODateWithMs)},{"lastResult",j.lastResult}};QJsonArray sources;for(auto&s:j.sources)sources.append(s);o["sources"]=sources;array.append(o);}QSaveFile f(configFilePath_);if(!f.open(QIODevice::WriteOnly)){if(saveError)*saveError=tr("计划保存失败：%1 (%2)").arg(f.fileName(),f.errorString());return false;}const QByteArray payload=QJsonDocument(QJsonObject{{"version",2},{"jobs",array}}).toJson();const qint64 written=f.write(payload);if(written!=payload.size()){qWarning("JobManager config save failed: %s (%s)",qPrintable(f.fileName()),qPrintable(f.errorString()));if(saveError)*saveError=f.errorString();f.cancelWriting();return false;}if(!f.commit()){if(saveError)*saveError=tr("计划保存失败：%1 (%2)").arg(f.fileName(),f.errorString());return false;}return true;}
void JobManager::load(){
    QFile file(configFilePath_);
    if(!file.exists())return;
    loadError_=tr("无法读取或解析备份计划：%1").arg(configFilePath_);
    if(!file.open(QIODevice::ReadOnly))return;
    const QJsonDocument document=QJsonDocument::fromJson(file.readAll());
    if(!document.isObject())return;
    const QJsonObject root=document.object();
    if(root.value("version").toInt(-1)!=2||!root.value("jobs").isArray())return;

    const auto hasKeys=[](const QJsonObject&object,const QStringList&keys){
        for(const QString&key:keys)if(!object.contains(key))return false;
        return true;
    };
    const QStringList jobKeys={"id","name","destination","cron","filter","enabled","realtime","retention",
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
        job.repository=object.value("repository").toBool(true);
        job.encrypted=object.value("encrypted").toBool(true);
        job.format=static_cast<PackFormat>(object.value("format").toInt(0));
        job.compression=static_cast<Compression>(object.value("compression").toInt(3));
        job.id=object.value("id").toString();
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9-]+$")).match(job.id).hasMatch()) return;
        job.name=object.value("name").toString();
        job.destination=object.value("destination").toString();
        job.cron=object.value("cron").toString();
        job.scheduled=object.contains("scheduled") ? object.value("scheduled").toBool() : true;
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
        if (filter.contains("sourceRules")) {
            if (!filter.value("sourceRules").isArray()) return;
            for (const auto &value : filter.value("sourceRules").toArray()) {
                if (!value.isObject()) return;
                const auto rule = value.toObject();
                if (!rule.value("include").isBool() || !rule.value("path").isString()) return;
                job.filter.sourceRules.push_back({rule.value("path").toString().toUtf8().toStdString(), rule.value("include").toBool()});
            }
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
    loadError_.clear();
}

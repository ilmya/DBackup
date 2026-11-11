#include "automation/job_manager.h"
#include "automation/cron.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {
// 返回应用数据目录中的计划文件路径，并确保目录已创建。
QString filePath() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return directory + "/jobs.json";
}

} // namespace

namespace job_policy {
// 检查计划标识仅由安全 ASCII 字母、数字和连字符组成。
bool validId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9-]+$"));
    return !id.isEmpty() && pattern.match(id).hasMatch();
}

// 校验计划必填项、运行模式、cron、筛选器和算法枚举。
bool validate(const BackupJob& job, QString& error) {
    if (!job.repository && job.realtime) {
        error = QObject::tr("独立归档不支持实时备份");
        return false;
    }
    if (job.repository && !job.encrypted) {
        error = QObject::tr("版本仓库需要加密密码");
        return false;
    }

    std::string detail;
    if (job.scheduled) {
        CronExpression expression;
        if (!expression.parse(job.cron.toStdString(), detail)) {
            error = QString::fromUtf8(detail.c_str());
            return false;
        }
        if (expression.next(QDateTime::currentSecsSinceEpoch()) == static_cast<std::time_t>(-1)) {
            error = QObject::tr("未来八年内无法找到符合 Cron 的运行时间");
            return false;
        }
    }
    if (job.name.trimmed().isEmpty() || job.sources.isEmpty() || job.destination.isEmpty() ||
        job.retention < 1) {
        error = QObject::tr("计划名称、来源、目标和保留数量无效");
        return false;
    }
    if (!ValidateFilterOptions(job.filter, detail)) {
        error = QString::fromUtf8(detail.c_str());
        return false;
    }
    if (static_cast<int>(job.format) > 1 || static_cast<int>(job.compression) > 3) {
        error = QObject::tr("无效的归档格式或压缩算法");
        return false;
    }

    error.clear();
    return true;
}
RunAction request(bool paused, bool hasExecutor, bool hasId, bool running, bool pending) {
    if (paused || !hasExecutor || !hasId)
        return RunAction::Reject;
    if (!running)
        return RunAction::Start;
    return pending ? RunAction::Merge : RunAction::Queue;
}
bool rerun(bool pending, bool found, bool enabled, bool cancelled) {
    return pending && found && enabled && !cancelled;
}
} // namespace job_policy

JobManager::JobManager(QObject* p, QString configFilePath)
    : QObject(p),
      configFilePath_(configFilePath.isEmpty() ? filePath() : std::move(configFilePath)) {
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    load();
}
void JobManager::start() {
    timer_.start(30000);
    if (watchers_.empty())
        syncWatchers();
    tick();
}
void JobManager::pause(bool value) {
    paused_ = value;
    if (!paused_)
        flushRebuildRescans();
}
bool JobManager::upsert(BackupJob job, QString& error) {
    if (!loadError_.isEmpty()) {
        error = loadError_;
        return false;
    }
    if (!job_policy::validate(job, error))
        return false;
    if (job.id.isEmpty())
        job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!job_policy::validId(job.id)) {
        error = tr("计划标识无效");
        return false;
    }
    updateNext(job);
    const auto before = jobs_;
    bool replaced = false;
    for (auto& existing : jobs_)
        if (existing.id == job.id) {
            existing = job;
            replaced = true;
            break;
        }
    if (!replaced)
        jobs_.append(job);
    if (!save(&error)) {
        jobs_ = before;
        return false;
    }
    syncWatchers(job.id);
    if (onChanged_)
        onChanged_();
    return true;
}
bool JobManager::remove(const QString& id, QString& error) {
    const auto before = jobs_;
    int index = -1;
    for (int i = 0; i < jobs_.size(); ++i)
        if (jobs_[i].id == id) {
            index = i;
            break;
        }
    if (index < 0) {
        error = tr("任务不存在");
        return false;
    }
    jobs_.removeAt(index);
    if (!save(&error)) {
        jobs_ = before;
        return false;
    }
    pending_.remove(id);
    pendingJobs_.remove(id);
    rebuildRescans_.remove(id);
    syncWatchers(id);
    if (onChanged_)
        onChanged_();
    return true;
}
bool JobManager::setEnabled(const QString& id, bool enabled, QString& error) {
    for (auto& job : jobs_)
        if (job.id == id) {
            BackupJob changed = job;
            changed.enabled = enabled;
            if (!upsert(changed, error))
                return false;
            if (!enabled) {
                pending_.remove(id);
                pendingJobs_.remove(id);
            }
            return true;
        }
    error = tr("任务不存在");
    return false;
}
bool JobManager::runNow(const QString& id, QString& error) {
    for (const auto& job : jobs_)
        if (job.id == id) {
            if (paused_) {
                error = tr("自动任务已暂停，无法立即运行");
                return false;
            }
            if (!executor_) {
                error = tr("备份执行器尚未就绪");
                return false;
            }
            const bool alreadyRunning = running_.contains(id);
            if (!requestRun(job)) {
                error = tr("任务未能加入运行队列");
                return false;
            }
            if (alreadyRunning)
                error = tr("任务正在运行，已排队一次");
            return true;
        }
    error = tr("任务不存在");
    return false;
}
void JobManager::updateNext(BackupJob& job) {
    if (!job.enabled || !job.scheduled) {
        job.nextRun = {};
        return;
    }
    CronExpression c;
    std::string e;
    if (c.parse(job.cron.toStdString(), e)) {
        const QDateTime now = clock_ ? clock_() : QDateTime::currentDateTime();
        const auto next = c.next(now.toSecsSinceEpoch());
        if (next != -1)
            job.nextRun = QDateTime::fromSecsSinceEpoch(next);
    }
}
bool JobManager::requestRun(const BackupJob& job) {
    const auto action = job_policy::request(paused_, bool(executor_), !job.id.isEmpty(),
                                            running_.contains(job.id), pending_.contains(job.id));
    if (action == job_policy::RunAction::Reject)
        return false;
    if (action != job_policy::RunAction::Start) {
        if (action == job_policy::RunAction::Queue)
            pendingJobs_[job.id] = job;
        else {
            auto& queued = pendingJobs_[job.id];
            if (!queued.realtimeTriggered || !job.realtimeTriggered) {
                queued.realtimeTriggered = false;
                queued.changedPaths.clear();
                queued.fullRescan = false;
            } else {
                queued.fullRescan |= job.fullRescan;
                for (const auto& path : job.changedPaths)
                    if (!queued.changedPaths.contains(path))
                        queued.changedPaths << path;
            }
        }
        pending_.insert(job.id);
        return true;
    }
    running_.insert(job.id);
    executor_(job);
    return true;
}
void JobManager::tick() {
    if (paused_ || !executor_)
        return;
    const QDateTime now = clock_ ? clock_() : QDateTime::currentDateTime();
    bool changed = false;
    for (auto& job : jobs_)
        if (job.enabled && job.nextRun.isValid() && job.nextRun <= now) {
            requestRun(job);
            updateNext(job);
            changed = true;
        }
    if (changed)
        save();
}
void JobManager::syncWatchers(const QString& changedId) {
    QSet<QString> affected;
    if (!changedId.isEmpty())
        affected.insert(changedId);
    else
        for (const auto& job : jobs_)
            affected.insert(job.id);

    for (const auto& id : affected) {
        const std::string prefix = id.toUtf8().toStdString() + "|";
        bool hadWatcher = false;
        ++watcherGeneration_[id.toUtf8().toStdString()];
        for (auto it = watchers_.begin(); it != watchers_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) {
                hadWatcher = true;
                realtimeGeneration_.erase(it->first);
                realtimeFirst_.erase(it->first);
                const QString key = QString::fromStdString(it->first);
                realtimeChanges_.remove(key);
                realtimeRescans_.remove(key);
                it = watchers_.erase(it);
            } else
                ++it;
        }
        if (hadWatcher)
            rebuildRescans_.insert(id);
        watcherStatuses_.remove(id);
    }

    for (auto& job : jobs_) {
        if (!affected.contains(job.id))
            continue;
        const std::string jobId = job.id.toUtf8().toStdString();
        const int watcherEpoch = ++watcherGeneration_[jobId];
        watcherStatuses_.remove(job.id);
        if (!job.enabled || !job.realtime) {
            job.realtimeStatus = job.realtime ? tr("监控已暂停") : tr("未启用实时备份");
            continue;
        }

        job.realtimeStatus = tr("正在连接监控目录");
        const QString destination = QDir::cleanPath(job.destination);
        for (const QString& source : job.sources) {
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
            const bool started = watcher->start(
                sourceUtf8,
                [this, id = job.id, key, excluded, selectedFile, sourceRoot,
                 watcherEpoch](const std::vector<std::string>& paths, bool fullRescan) {
                    if (!selectedFile.isEmpty() && !fullRescan) {
                        bool relevant = false;
                        for (const auto& path : paths)
                            if (QString::fromUtf8(path.c_str()) == selectedFile) {
                                relevant = true;
                                break;
                            }
                        if (!relevant)
                            return;
                    }
                    if (!excluded.isEmpty() && !paths.empty() && !fullRescan) {
                        bool onlyOutput = true;
                        for (const auto& path : paths) {
                            const QString changed = QString::fromUtf8(path.c_str());
                            if (!changed.startsWith(excluded + "/") && changed != excluded) {
                                onlyOutput = false;
                                break;
                            }
                        }
                        if (onlyOutput)
                            return;
                    }

                    QStringList changedPaths;
                    for (const auto& path : paths)
                        changedPaths
                            << QDir(sourceRoot).absoluteFilePath(QString::fromUtf8(path.c_str()));
                    QMetaObject::invokeMethod(
                        this,
                        [this, id, key, watcherEpoch, changedPaths, fullRescan] {
                            const auto currentEpoch =
                                watcherGeneration_.find(id.toUtf8().toStdString());
                            if (currentEpoch == watcherGeneration_.end() ||
                                currentEpoch->second != watcherEpoch)
                                return;

                            const QString changeKey = QString::fromStdString(key);
                            for (const auto& path : changedPaths)
                                realtimeChanges_[changeKey].insert(QDir::cleanPath(path));
                            if (fullRescan)
                                realtimeRescans_.insert(changeKey);
                            const int generation = ++realtimeGeneration_[key];
                            if (!realtimeFirst_[key].isValid()) {
                                realtimeFirst_[key] = QDateTime::currentDateTime();
                                const QDateTime first = realtimeFirst_[key];
                                QTimer::singleShot(
                                    30000, this, [this, id, key, first, watcherEpoch] {
                                        const auto epoch =
                                            watcherGeneration_.find(id.toUtf8().toStdString());
                                        if (epoch == watcherGeneration_.end() ||
                                            epoch->second != watcherEpoch || paused_ ||
                                            realtimeFirst_[key] != first || !executor_)
                                            return;
                                        realtimeFirst_[key] = {};
                                        ++realtimeGeneration_[key];
                                        for (const auto& candidate : jobs_) {
                                            if (candidate.id == id && candidate.enabled) {
                                                BackupJob run = candidate;
                                                run.realtimeTriggered = true;
                                                const QString changeKey =
                                                    QString::fromStdString(key);
                                                run.changedPaths =
                                                    realtimeChanges_.take(changeKey).values();
                                                run.fullRescan =
                                                    realtimeRescans_.remove(changeKey) > 0;
                                                run.changedPaths.sort();
                                                requestRun(run);
                                                break;
                                            }
                                        }
                                    });
                            }
                            QTimer::singleShot(
                                3000, this, [this, id, key, generation, watcherEpoch] {
                                    const auto epoch =
                                        watcherGeneration_.find(id.toUtf8().toStdString());
                                    if (epoch == watcherGeneration_.end() ||
                                        epoch->second != watcherEpoch || paused_ ||
                                        realtimeGeneration_[key] != generation || !executor_)
                                        return;
                                    realtimeFirst_[key] = {};
                                    for (const auto& candidate : jobs_) {
                                        if (candidate.id == id && candidate.enabled) {
                                            BackupJob run = candidate;
                                            run.realtimeTriggered = true;
                                            const QString changeKey = QString::fromStdString(key);
                                            run.changedPaths =
                                                realtimeChanges_.take(changeKey).values();
                                            run.fullRescan = realtimeRescans_.remove(changeKey) > 0;
                                            run.changedPaths.sort();
                                            requestRun(run);
                                            break;
                                        }
                                    }
                                });
                        },
                        Qt::QueuedConnection);
                },
                error, 0, 0,
                [this, id = job.id, selectedPath, watcherEpoch](bool available,
                                                                const std::string& message) {
                    const QString status =
                        available && message.find("部分目录无法监控") == std::string::npos
                            ? tr("监控中")
                            : QString::fromUtf8(message.c_str());
                    QMetaObject::invokeMethod(
                        this,
                        [this, id, selectedPath, watcherEpoch, status] {
                            const auto epoch = watcherGeneration_.find(id.toUtf8().toStdString());
                            if (epoch == watcherGeneration_.end() || epoch->second != watcherEpoch)
                                return;
                            watcherStatuses_[id][selectedPath] = status;
                            QString aggregate = tr("监控中");
                            for (const auto& value : watcherStatuses_[id])
                                if (value != tr("监控中")) {
                                    aggregate = value;
                                    break;
                                }
                            for (auto& candidate : jobs_) {
                                if (candidate.id == id) {
                                    candidate.realtimeStatus = aggregate;
                                    break;
                                }
                            }
                            flushRebuildRescans();
                            if (onChanged_)
                                onChanged_();
                        },
                        Qt::QueuedConnection);
                },
                directory);

            if (started && watcher->running())
                watchers_[key] = std::move(watcher);
            else {
                job.realtimeStatus = QString::fromUtf8(error.c_str());
                watcherStatuses_[job.id][selectedPath] = job.realtimeStatus;
            }
        }
    }
    flushRebuildRescans();
}
void JobManager::flushRebuildRescans() {
    if (paused_ || !executor_)
        return;
    const auto ids = rebuildRescans_;
    for (const auto& id : ids) {
        bool found = false;
        for (const auto& job : jobs_)
            if (job.id == id && job.enabled && job.realtime) {
                found = true;
                bool ready = !job.sources.isEmpty();
                for (const auto& source : job.sources) {
                    const QString selected = QDir::cleanPath(source);
                    if (watcherStatuses_.value(id).value(selected) != tr("监控中")) {
                        ready = false;
                        break;
                    }
                }
                if (!ready)
                    break;
                BackupJob run = job;
                run.realtimeTriggered = true;
                run.fullRescan = true;
                rebuildRescans_.remove(id);
                requestRun(run);
                break;
            }
        if (!found)
            rebuildRescans_.remove(id);
    }
}
void JobManager::markFinished(const QString& id, bool success, const QString& message, bool partial,
                              bool cancelled) {
    running_.remove(id);
    BackupJob rerun;
    bool found = false;
    const bool hasQueuedRun = pending_.remove(id) > 0;
    const BackupJob queued = pendingJobs_.take(id);
    for (auto& job : jobs_)
        if (job.id == id) {
            job.lastRun = clock_ ? clock_() : QDateTime::currentDateTime();
            if (success && !partial && !cancelled)
                job.lastSuccess = job.lastRun;
            job.lastResult = (cancelled ? tr("已取消：")
                                        : (success ? (partial ? tr("部分完成：") : tr("成功："))
                                                   : tr("失败："))) +
                             message;
            if (job.enabled)
                updateNext(job);
            else
                job.nextRun = {};
            rerun = job;
            found = true;
            break;
        }
    if (found) {
        save();
        if (onChanged_)
            onChanged_();
    }
    if (job_policy::rerun(hasQueuedRun, found, rerun.enabled, cancelled)) {
        rerun.realtimeTriggered = queued.realtimeTriggered;
        rerun.changedPaths = queued.changedPaths;
        rerun.fullRescan = queued.fullRescan;
        requestRun(rerun);
    }
}
bool JobManager::save(QString* saveError) {
    QJsonArray array;
    for (const auto& j : jobs_) {
        QJsonObject filter{{"pathContains", QString::fromUtf8(j.filter.pathContains.c_str())},
                           {"nameContains", QString::fromUtf8(j.filter.nameContains.c_str())},
                           {"extension", QString::fromUtf8(j.filter.extension.c_str())},
                           {"ownerContains", QString::fromUtf8(j.filter.ownerContains.c_str())},
                           {"type", static_cast<int>(j.filter.type)},
                           {"typeMask", static_cast<int>(j.filter.typeMask)},
                           {"minSize", static_cast<double>(j.filter.minSize)},
                           {"maxSize", static_cast<double>(j.filter.maxSize)},
                           {"afterMtimeMs", static_cast<double>(j.filter.afterMtimeMs)},
                           {"beforeMtimeMs", static_cast<double>(j.filter.beforeMtimeMs)}};
        QJsonArray extensions;
        for (const auto& s : j.filter.extensions)
            extensions.append(QString::fromUtf8(s.c_str()));
        filter["extensions"] = extensions;
        QJsonArray rules;
        for (const auto& r : j.filter.pathRules)
            rules.append(QJsonObject{{"include", r.include},
                                     {"pattern", QString::fromUtf8(r.pattern.c_str())}});
        filter["pathRules"] = rules;
        QJsonArray selected;
        for (const auto& r : j.filter.sourceRules)
            selected.append(
                QJsonObject{{"include", r.include}, {"path", QString::fromUtf8(r.path.c_str())}});
        filter["sourceRules"] = selected;
        QJsonObject o{{"repository", j.repository},
                      {"encrypted", j.encrypted},
                      {"format", static_cast<int>(j.format)},
                      {"compression", static_cast<int>(j.compression)},
                      {"id", j.id},
                      {"name", j.name},
                      {"destination", j.destination},
                      {"cron", j.cron},
                      {"filter", filter},
                      {"enabled", j.enabled},
                      {"scheduled", j.scheduled},
                      {"realtime", j.realtime},
                      {"retention", j.retention},
                      {"lastRun", j.lastRun.toString(Qt::ISODateWithMs)},
                      {"lastSuccess", j.lastSuccess.toString(Qt::ISODateWithMs)},
                      {"nextRun", j.nextRun.toString(Qt::ISODateWithMs)},
                      {"lastResult", j.lastResult}};
        QJsonArray sources;
        for (auto& s : j.sources)
            sources.append(s);
        o["sources"] = sources;
        array.append(o);
    }
    QSaveFile f(configFilePath_);
    if (!f.open(QIODevice::WriteOnly)) {
        if (saveError)
            *saveError = tr("计划保存失败：%1 (%2)").arg(f.fileName(), f.errorString());
        return false;
    }
    const QByteArray payload = QJsonDocument(QJsonObject{{"version", 2}, {"jobs", array}}).toJson();
    const qint64 written = f.write(payload);
    if (written != payload.size()) {
        qWarning("JobManager config save failed: %s (%s)", qPrintable(f.fileName()),
                 qPrintable(f.errorString()));
        if (saveError)
            *saveError = f.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (saveError)
            *saveError = tr("计划保存失败：%1 (%2)").arg(f.fileName(), f.errorString());
        return false;
    }
    return true;
}
void JobManager::load() {
    QFile file(configFilePath_);
    if (!file.exists())
        return;
    loadError_ = tr("无法读取或解析备份计划：%1").arg(configFilePath_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    const QJsonObject root = document.object();
    if (root.value("version").toInt(-1) != 2 || !root.value("jobs").isArray())
        return;

    const auto hasKeys = [](const QJsonObject& object, const QStringList& keys) {
        for (const QString& key : keys)
            if (!object.contains(key))
                return false;
        return true;
    };
    const QStringList jobKeys = {"id",      "name",    "destination", "cron",
                                 "filter",  "enabled", "realtime",    "retention",
                                 "lastRun", "nextRun", "lastResult",  "sources"};
    const QStringList filterKeys = {"pathContains",  "nameContains", "extension", "ownerContains",
                                    "type",          "minSize",      "maxSize",   "afterMtimeMs",
                                    "beforeMtimeMs", "extensions",   "pathRules"};

    QList<BackupJob> loaded;
    for (const QJsonValue& value : root.value("jobs").toArray()) {
        if (!value.isObject())
            return;
        const QJsonObject object = value.toObject();
        if (!hasKeys(object, jobKeys) || !object.value("filter").isObject() ||
            !object.value("sources").isArray())
            return;
        const QJsonObject filter = object.value("filter").toObject();
        if (!hasKeys(filter, filterKeys) || !filter.value("extensions").isArray() ||
            !filter.value("pathRules").isArray())
            return;

        BackupJob job;
        job.repository = object.value("repository").toBool(true);
        job.encrypted = object.value("encrypted").toBool(true);
        job.format = static_cast<PackFormat>(object.value("format").toInt(0));
        job.compression = static_cast<Compression>(object.value("compression").toInt(3));
        job.id = object.value("id").toString();
        if (!job_policy::validId(job.id))
            return;
        job.name = object.value("name").toString();
        job.destination = object.value("destination").toString();
        job.cron = object.value("cron").toString();
        job.scheduled = object.contains("scheduled") ? object.value("scheduled").toBool() : true;
        job.filter.pathContains = filter.value("pathContains").toString().toUtf8().toStdString();
        job.filter.nameContains = filter.value("nameContains").toString().toUtf8().toStdString();
        job.filter.extension = filter.value("extension").toString().toUtf8().toStdString();
        job.filter.ownerContains = filter.value("ownerContains").toString().toUtf8().toStdString();
        job.filter.type = static_cast<EntryTypeFilter>(filter.value("type").toInt());
        job.filter.typeMask = static_cast<uint32_t>(filter.value("typeMask").toInt(255));
        job.filter.minSize = static_cast<uint64_t>(filter.value("minSize").toDouble());
        job.filter.maxSize = static_cast<uint64_t>(filter.value("maxSize").toDouble());
        job.filter.afterMtimeMs = static_cast<int64_t>(filter.value("afterMtimeMs").toDouble());
        job.filter.beforeMtimeMs = static_cast<int64_t>(filter.value("beforeMtimeMs").toDouble());
        for (const QJsonValue& extension : filter.value("extensions").toArray()) {
            if (!extension.isString())
                return;
            job.filter.extensions.push_back(extension.toString().toUtf8().toStdString());
        }
        for (const QJsonValue& ruleValue : filter.value("pathRules").toArray()) {
            if (!ruleValue.isObject())
                return;
            const QJsonObject rule = ruleValue.toObject();
            if (!rule.value("include").isBool() || !rule.value("pattern").isString())
                return;
            job.filter.pathRules.push_back(
                {rule.value("include").toBool(),
                 rule.value("pattern").toString().toUtf8().toStdString()});
        }
        if (filter.contains("sourceRules")) {
            if (!filter.value("sourceRules").isArray())
                return;
            for (const auto& value : filter.value("sourceRules").toArray()) {
                if (!value.isObject())
                    return;
                const auto rule = value.toObject();
                if (!rule.value("include").isBool() || !rule.value("path").isString())
                    return;
                job.filter.sourceRules.push_back(
                    {rule.value("path").toString().toUtf8().toStdString(),
                     rule.value("include").toBool()});
            }
        }
        job.enabled = object.value("enabled").toBool();
        job.realtime = object.value("realtime").toBool();
        job.retention = object.value("retention").toInt();
        job.lastRun = QDateTime::fromString(object.value("lastRun").toString(), Qt::ISODateWithMs);
        job.lastResult = object.value("lastResult").toString();
        if (object.contains("lastSuccess"))
            job.lastSuccess = QDateTime::fromString(object.value("lastSuccess").toString(), Qt::ISODateWithMs);
        else if (job.lastResult.startsWith(tr("成功：")))
            job.lastSuccess = job.lastRun;
        for (const QJsonValue& source : object.value("sources").toArray()) {
            if (!source.isString())
                return;
            job.sources << source.toString();
        }
        updateNext(job);
        loaded << job;
    }
    jobs_ = std::move(loaded);
    loadError_.clear();
}

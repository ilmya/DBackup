// 按计划执行备份并处理版本保留
// 根据计划类型调用完整备份或增量版本链，仅在成功提交后执行对应的历史保留策略。
#pragma once

#include "automation/job_manager.h"
#include "core/incremental/incremental_chain.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QUuid>

// 记录计划执行是否成功、是否部分完成，以及增量版本的文件变化。
struct PlanResult {
    bool success = false;
    bool partial = false;
    std::string detail; // 成功时为结果路径，失败或部分完成时为诊断信息
    bool changesAvailable = false; // 只有增量版本提供可信的本次文件变化集合
    std::vector<std::string> added, modified, deleted;
};

// 按计划执行完整或增量备份，并仅在完整成功时执行保留清理
inline PlanResult executePlan(const BackupJob& job, const std::string& password,
                              OperationContext context = {}) {
    context.includeSourceDirectory = true;
    PlanResult result;
    std::vector<std::string> sources;
    for (const auto& source : job.sources)
        sources.push_back(source.toUtf8().toStdString());
    if (job.tarIncremental) {
        const QString chain = QDir(job.destination).filePath("dbackup-" + job.id + "-tar-incremental");
        PackOptions options;
        options.format = job.format;
        options.compression = job.compression;
        options.password = password;
        options.filter = job.filter;
        dbackup::incremental::CreatedVersion version;
        result.success = dbackup::incremental::createVersion(
            chain.toUtf8().toStdString(), sources, options, context, version, result.detail);
        if (!result.success)
            return result;
        result.changesAvailable = true;
        result.added = std::move(version.added);
        result.modified = std::move(version.modified);
        result.deleted = std::move(version.deleted);
        if (context.isCancelled()) {
            result.success = false;
            result.detail = "操作已取消；已提交的备份版本保留，未清理旧版本";
            return result;
        }
        size_t removed = 0;
        std::string pruneError;
        if (!dbackup::incremental::pruneVersions(chain.toUtf8().toStdString(), password,
                                                  job.retention, context, removed, pruneError, &options)) {
            result.success = false;
            result.detail = "备份版本已提交，但清理旧版本失败: " + pruneError;
            return result;
        }
        result.detail = chain.toUtf8().toStdString();
        return result;
    }
    context.noReplace = true;
    const bool tar =
        job.format == PackFormat::Tar && job.compression == Compression::None && !job.encrypted;
    const QString prefix = "dbackup-" + job.id + "-";
    const QString filename =
        prefix + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + (tar ? ".tar" : ".abk");
    const QString output = QDir(job.destination).filePath(filename);
    PackOptions options;
    options.format = job.format;
    options.compression = job.compression;
    options.password = password;
    options.filter = job.filter;
    result.success = ArchiveService::create(sources, output.toUtf8().toStdString(), options,
                                            result.detail, context);
    result.partial = result.success && !result.detail.empty();
    if (result.success && context.isCancelled()) {
        result.success = false;
        result.detail = "操作已取消；已提交的备份保留，未清理旧备份";
        return result;
    }
    if (result.success && !result.partial) {
        QDir directory(job.destination);
        const auto files = directory.entryInfoList({prefix + "*.tar", prefix + "*.abk"},
                                                   QDir::Files | QDir::NoSymLinks, QDir::Time);
        const QRegularExpression managed(
            QStringLiteral("^%1[0-9]{8}-[0-9]{6}-[0-9]{3}-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-"
                           "9a-f]{4}-[0-9a-f]{12}\\.(tar|abk)$")
                .arg(QRegularExpression::escape(prefix)));
        int kept = 1;
        for (const auto& file : files) {
            if (!managed.match(file.fileName()).hasMatch())
                continue;
            if (file.absoluteFilePath() == QFileInfo(output).absoluteFilePath())
                continue;
            if (kept++ < job.retention)
                continue;
            if (!QFile::remove(file.absoluteFilePath())) {
                result.detail =
                    "无法删除计划的旧备份: " + file.absoluteFilePath().toUtf8().toStdString();
                result.success = false;
                break;
            }
        }
    }
    if (result.success && !result.partial)
        result.detail = output.toUtf8().toStdString();
    return result;
}

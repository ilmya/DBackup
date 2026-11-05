#pragma once

#include "automation/job_manager.h"
#include "core/repository.h"
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QRegularExpression>

struct PlanResult {
    bool success=false;
    bool partial=false;
    std::string detail;
};

inline PlanResult executePlan(const BackupJob &job, const std::string &password, OperationContext context={}) {
    PlanResult result;
    std::vector<std::string> sources;
    for (const auto &source:job.sources) sources.push_back(source.toUtf8().toStdString());
    if (job.repository) {
        SnapshotOptions options; options.sources=sources; options.password=password; options.filter=job.filter;
        LocalRepository repository(job.destination.toUtf8().toStdString()); SnapshotInfo snapshot;
        result.success=repository.createSnapshot(options,snapshot,result.detail,context);
        result.partial=snapshot.partial;
        if(result.success && context.isCancelled()){result.success=false;result.detail="操作已取消；已提交的快照保留，未清理旧快照";return result;}
        if(result.success && !result.partial) result.success=repository.prune(job.retention,result.detail);
        return result;
    }
    context.noReplace=true;
    const bool tar=job.format==PackFormat::Tar && job.compression==Compression::None && !job.encrypted;
    const QString prefix="dbackup-"+job.id+"-";
    const QString filename=prefix+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+(tar?".tar":".abk");
    const QString output=QDir(job.destination).filePath(filename);
    PackOptions options;options.format=job.format;options.compression=job.compression;options.password=password;options.filter=job.filter;
    result.success=ArchiveService::create(sources,output.toUtf8().toStdString(),options,result.detail,context);
    result.partial=result.success && !result.detail.empty();
    if(result.success && context.isCancelled()){result.success=false;result.detail="操作已取消；已提交的归档保留，未清理旧归档";return result;}
    if(result.success && !result.partial) {
        QDir directory(job.destination);
        const auto files=directory.entryInfoList({prefix+"*.tar",prefix+"*.abk"},QDir::Files|QDir::NoSymLinks,QDir::Time);
        const QRegularExpression managed(QStringLiteral("^%1[0-9]{8}-[0-9]{6}-[0-9]{3}-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(tar|abk)$").arg(QRegularExpression::escape(prefix)));
        int kept=1;
        for(const auto &file:files) {
            if(!managed.match(file.fileName()).hasMatch())continue;
            if(file.absoluteFilePath()==QFileInfo(output).absoluteFilePath())continue;
            if(kept++<job.retention)continue;
            if(!QFile::remove(file.absoluteFilePath())) {
                result.detail="无法删除计划的旧归档: "+file.absoluteFilePath().toUtf8().toStdString(); result.success=false; break;
            }
        }
    }
    if(result.success && !result.partial) result.detail=output.toUtf8().toStdString();
    return result;
}

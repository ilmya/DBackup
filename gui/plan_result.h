#pragma once
#include "automation/job_manager.h"
#include <QObject>

inline QString realtimeResultDetail(const BackupJob &job, const QString &detail, bool partial) {
    QString result=QObject::tr("本次变化：%1").arg(job.changedPaths.isEmpty()?QObject::tr("未获取路径，已重新扫描"):job.changedPaths.join(QObject::tr("、")));
    if(job.fullRescan && !job.changedPaths.isEmpty())result+=QObject::tr("（已重新扫描）");
    const auto warnings=detail.split('\n',Qt::SkipEmptyParts);
    QStringList relevant;
    for(const auto &line:warnings)for(const auto &path:job.changedPaths)if(line.contains(path)){relevant<<line;break;}
    if(!relevant.isEmpty())result+=QObject::tr("；变更项提示：%1").arg(relevant.join(QObject::tr("；")));
    if(partial && relevant.size()<warnings.size())result+=QObject::tr("；存在其他未备份项，详见活动日志和快照");
    return result;
}

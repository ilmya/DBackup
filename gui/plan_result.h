// 计划执行结果和实时任务提示的展示
// 将计划运行结果和文件变化整理为界面可读文本，供任务列表与详情复用。
#pragma once
#include "automation/job_manager.h"
#include <QObject>

// 从详细日志中提取实时备份变更项摘要，并保留部分完成提示
inline QString realtimeResultDetail(const BackupJob& job, const QString& detail, bool partial) {
    QString result =
        QObject::tr("本次变化：%1")
            .arg(job.changedPaths.isEmpty() ? QObject::tr("未获取路径，已重新扫描")
                                            : job.changedPaths.join(QObject::tr("、")));
    if (job.fullRescan && !job.changedPaths.isEmpty())
        result += QObject::tr("（已重新扫描）");
    const auto warnings = detail.split('\n', Qt::SkipEmptyParts);
    QStringList relevant;
    for (const auto& line : warnings)
        for (const auto& path : job.changedPaths)
            if (line.contains(path)) {
                relevant << line;
                break;
            }
    if (!relevant.isEmpty())
        result += QObject::tr("；变更项提示：%1").arg(relevant.join(QObject::tr("；")));
    if (partial && relevant.size() < warnings.size())
        result += QObject::tr("；存在其他未备份项，详见活动日志和计划详情");
    return result;
}

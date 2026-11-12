// 声明文件级增量备份的写入、验证和提取接口
// 约定增量版本中选定文件的打包、内容验证和相对目标目录提取方式。
// 按已扫描的选定路径生成备份，并与已认证清单核对后恢复
#pragma once

#include "core/archive/archive_internal.h"
#include "core/incremental/incremental_manifest.h"

namespace dbackup::incremental {

// 将完整扫描中选定的路径写入自定义或 tar 备份；调用方另行提交清单
// 来源读取不完整时不将备份作为成功结果发布
bool writeSelectedArchive(const std::vector<archive_scan::Entry>& scanned,
                          const std::vector<std::string>& selected,
                          const std::string& destination, const PackOptions& options,
                          const OperationContext& context, std::string& error);

// 对照清单验证备份元数据和普通文件的 SHA-256 摘要
// 加密清单须预先认证；调用方须防止校验后的备份在恢复前被替换
bool verifyArchive(const std::string& path, const std::string& password,
                   const Manifest& current, const Manifest* previous,
                   const OperationContext& context, std::string& error);

// 相对于已打开的目录描述符提取事先验证过的备份
bool extractArchiveToFd(const std::string& path, int destinationFd, const std::string& password,
                        const OperationContext& context, std::string& error);

} // namespace dbackup::incremental

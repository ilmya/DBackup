// 管理文件级增量备份清单
// 定义完整文件状态清单和保护、解析、交叉验证接口，记录版本顺序与删除项。
// 编码、解码和保护完整文件状态，校验版本依赖、删除记录及备份条目
#pragma once

#include "core/incremental/incremental_diff.h"
#include "core/archive/archive.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::incremental {

// 每份清单保存备份完成后的完整文件状态和明确的删除记录
struct Manifest {
    // 读取版本链时填入目录名；该字段不写入清单
    std::string directoryName;
    uint64_t sequence = 0;       // 当前版本序号，必须为非零值
    uint64_t previous = 0;       // 依赖的前一版本；完整版本为零
    int64_t createdAtMs = 0;     // 创建时刻，自 Unix 纪元起的毫秒数
    bool full = false;          // 标识完整版本或增量版本
    std::vector<FileState> files; // 当前版本备份完成后的完整文件状态
    std::vector<std::string> deleted; // 相对前一版本删除的路径，完整版本为空
};

// 将清单编码并附带校验和；加密备份的调用方还须认证或加密编码结果
bool encodeManifest(const Manifest& manifest, std::vector<uint8_t>& bytes, std::string& error);
// 解码并校验明文清单；数据损坏或字段无效时返回错误
bool decodeManifest(const std::vector<uint8_t>& bytes, Manifest& manifest, std::string& error);
// 使用 AES-256-GCM 保护清单；加密版本不会隐式回退为明文
bool encodeProtectedManifest(const Manifest& manifest, const std::string& password,
                             std::vector<uint8_t>& bytes, std::string& error);
// 使用密码认证并解密清单；密码错误或数据被修改时拒绝
bool decodeProtectedManifest(const std::vector<uint8_t>& bytes, const std::string& password,
                             Manifest& manifest, std::string& error);

// 验证版本顺序、版本依赖及跨版本删除记录的一致性
bool validateChain(const std::vector<Manifest>& chain, std::string& error);

// 计算可删除的最早版本数量，同时保留至少 keepLatest 个版本及其依赖的完整备份
bool removablePrefix(const std::vector<Manifest>& chain, size_t keepLatest, size_t& count,
                     std::string& error);

// 对照版本清单检查备份条目元数据；此函数不认证文件内容或清单本身
bool validateArchiveEntries(const Manifest& current, const Manifest* previous,
                            const std::vector<ArchiveEntry>& entries, std::string& error);

} // namespace dbackup::incremental

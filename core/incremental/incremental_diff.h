// 比较两次备份的文件状态
// 定义文件状态和差异集合，供增量扫描、清单编码和版本创建共享。
// 校验相对路径和重复项，结合类型、元数据与内容摘要计算变化及删除列表
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dbackup::incremental {

// 扫描完整成功后生成的单个文件状态，用于比较版本差异
struct FileState {
    std::string path;
    uint8_t type = 0;
    uint32_t mode = 0, uid = 0, gid = 0;
    int64_t mtimeNs = 0;
    uint64_t size = 0;
    uint32_t deviceMajor = 0, deviceMinor = 0;
    std::string linkTarget;
    // 普通文件的内容摘要，可用于发现元数据未变化的内容修改
    std::string contentDigest;
};

// 保存与上一版本相比发生变化及被删除的路径
struct Changes {
    std::vector<std::string> changed;
    std::vector<std::string> deleted;
};

// 比较两个版本的状态并计算变化及删除路径；拒绝不安全或重复的路径，失败时不覆盖输出
bool compare(const std::vector<FileState>& previous, const std::vector<FileState>& current,
             Changes& changes, std::string& error);

} // namespace dbackup::incremental

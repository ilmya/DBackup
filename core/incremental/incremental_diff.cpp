// 比较两次备份的文件状态
// 比较前后两次扫描的路径、元数据和内容摘要，生成添加、修改与删除集合。
// 校验相对路径和重复项，结合类型、元数据与内容摘要计算变化及删除列表
#include "core/incremental/incremental_diff.h"
#include "core/filesystem/linux_file.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace dbackup::incremental {
namespace {
using Index = std::map<std::string, const FileState*>;

// 校验状态路径并构建索引，拒绝重复或不安全的路径
bool indexStates(const std::vector<FileState>& states, Index& index, std::string& error) {
    for (const auto& state : states) {
        const std::filesystem::path path(state.path);
        if (state.path.find('\0') != std::string::npos || !linux_file::validRelativePath(path) ||
            path.lexically_normal().generic_string() != state.path || state.type > 7 ||
            !index.emplace(state.path, &state).second) {
            error = "增量状态包含重复或不安全的路径: " + state.path;
            return false;
        }
    }
    return true;
}

// 比较文件类型、元数据、链接目标和内容摘要
bool same(const FileState& a, const FileState& b) {
    return std::tie(a.type, a.mode, a.uid, a.gid, a.mtimeNs, a.size, a.deviceMajor,
                    a.deviceMinor, a.linkTarget, a.contentDigest) ==
           std::tie(b.type, b.mode, b.uid, b.gid, b.mtimeNs, b.size, b.deviceMajor,
                    b.deviceMinor, b.linkTarget, b.contentDigest);
}
} // namespace

// 比较两次完整文件状态，输出变化路径及删除路径
bool compare(const std::vector<FileState>& previous, const std::vector<FileState>& current,
             Changes& changes, std::string& error) {
    Index before, after;
    error.clear();
    if (!indexStates(previous, before, error) || !indexStates(current, after, error))
        return false;
    Changes result;
    for (const auto& [path, state] : after) {
        auto old = before.find(path);
        if (old == before.end() || !same(*old->second, *state))
            result.changed.push_back(path);
    }
    for (const auto& [path, state] : before) {
        (void)state;
        if (!after.count(path))
            result.deleted.push_back(path);
    }
    changes = std::move(result);
    return true;
}
} // namespace dbackup::incremental

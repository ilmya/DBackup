#include "core/incremental_diff.h"
#include "core/linux_file.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace dbackup::incremental {
namespace {
using Index = std::map<std::string, const FileState*>;

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

bool same(const FileState& a, const FileState& b) {
    return std::tie(a.type, a.mode, a.uid, a.gid, a.mtimeNs, a.size, a.deviceMajor,
                    a.deviceMinor, a.linkTarget, a.contentDigest) ==
           std::tie(b.type, b.mode, b.uid, b.gid, b.mtimeNs, b.size, b.deviceMajor,
                    b.deviceMinor, b.linkTarget, b.contentDigest);
}
} // namespace

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

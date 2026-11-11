#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class OperationStage {
    Scanning,
    Reading,
    Compressing,
    Encrypting,
    Writing,
    Restoring,
    Completed
};

struct OperationProgress {
    OperationStage stage = OperationStage::Scanning;
    std::string currentPath;
    uint64_t completedFiles = 0;
    uint64_t totalFiles = 0;
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
    double bytesPerSecond = 0.0;
};

struct OperationContext {
    std::atomic_bool* cancelled = nullptr;
    std::function<void(const OperationProgress&)> onProgress;
    bool allowDeviceNodes = true;
    bool noReplace = false;
    bool includeSourceDirectory = false;
    // Internal staged restores can report socket notices after the final commit.
    bool suppressSocketNotice = false;
    std::vector<std::string>* warnings = nullptr;

    // 查询调用方是否请求取消当前操作。
    bool isCancelled() const {
        return cancelled && cancelled->load();
    }
    // 存在进度回调时向界面或调用方报告当前阶段。
    void report(const OperationProgress& progress) const {
        if (onProgress)
            onProgress(progress);
    }
    // 将可恢复问题写入警告列表，供部分完成报告使用。
    void warn(const std::string& message) const {
        if (warnings)
            warnings->push_back(message);
    }
};

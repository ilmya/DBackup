#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

enum class OperationStage { Scanning, Reading, Compressing, Encrypting, Writing, Uploading, Downloading, Restoring, Completed };

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
    std::atomic_bool *cancelled = nullptr;
    std::function<void(const OperationProgress &)> onProgress;

    bool isCancelled() const { return cancelled && cancelled->load(); }
    void report(const OperationProgress &progress) const { if (onProgress) onProgress(progress); }
};

// 定义备份与恢复操作共用的运行上下文
// 定义跨线程传递的操作上下文：取消标志、进度回调、恢复行为和可恢复问题列表。
// 统一传递执行阶段、进度回调、取消状态以及警告和恢复选项
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 标识备份或恢复当前执行的阶段
enum class OperationStage {
    Scanning,    // 枚举来源并应用筛选条件
    Reading,     // 读取文件内容或备份数据
    Compressing, // 执行所选压缩算法
    Encrypting,  // 生成加密数据及认证信息
    Writing,     // 写入备份或暂存文件
    Restoring,   // 重建文件及元数据
    Completed    // 本次操作处理完成
};

// 保存当前路径、文件及字节进度和传输速率
struct OperationProgress {
    OperationStage stage = OperationStage::Scanning;
    std::string currentPath; // 当前处理的来源或目标路径
    uint64_t completedFiles = 0;
    uint64_t totalFiles = 0;
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
    double bytesPerSecond = 0.0; // 当前报告的字节传输速率
};

// 由调用方提供取消、进度回调、恢复选项和警告收集
struct OperationContext {
    // 取消标志由调用方持有，必须在操作及后台回调结束前保持有效。
    std::atomic_bool* cancelled = nullptr;
    // 回调在执行操作的线程调用；图形界面须转发到界面线程后更新控件。
    std::function<void(const OperationProgress&)> onProgress;
    bool allowDeviceNodes = true; // 是否允许恢复字符设备和块设备节点
    bool noReplace = false;       // 提交时拒绝覆盖已有目标
    bool includeSourceDirectory = false; // 单个目录来源是否保留目录本身
    // 内部暂存恢复可在最终提交后再报告套接字提示
    bool suppressSocketNotice = false;
    std::vector<std::string>* warnings = nullptr; // 可选的非致命问题收集器，由调用方持有

    // 查询调用方是否请求取消当前操作
    bool isCancelled() const {
        return cancelled && cancelled->load();
    }
    // 存在进度回调时向界面或调用方报告当前阶段
    void report(const OperationProgress& progress) const {
        if (onProgress)
            onProgress(progress);
    }
    // 将可恢复问题写入警告列表，供部分完成报告使用
    void warn(const std::string& message) const {
        if (warnings)
            warnings->push_back(message);
    }
};

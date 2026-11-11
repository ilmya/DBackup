#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class DirectoryWatcher {
  public:
    using Callback = std::function<void(const std::vector<std::string>&, bool fullRescan)>;
    using StatusCallback = std::function<void(bool available, const std::string& message)>;
    // 初始化未启动的目录监控器。
    DirectoryWatcher();
    // 停止工作线程并释放监控资源。
    ~DirectoryWatcher();
    // 开始监控路径变化；路径失效时通过状态回调报告并尝试重连。
    bool start(const std::string& path, Callback callback, std::string& error,
               unsigned debounceMs = 3000, unsigned maximumDelayMs = 30000,
               StatusCallback statusCallback = {}, bool recursive = true);
    // 请求停止监控并等待工作线程退出。
    void stop();
    // 查询监控线程当前是否运行。
    bool running() const {
        return running_.load();
    }

  private:
    // 执行 inotify 事件循环、目录重连及变化去抖。
    void run();
    std::string path_;
    Callback callback_;
    StatusCallback statusCallback_;
    unsigned debounceMs_ = 3000, maximumDelayMs_ = 30000;
    bool recursive_ = true;
    std::atomic_bool running_{false};
    std::thread worker_;
};

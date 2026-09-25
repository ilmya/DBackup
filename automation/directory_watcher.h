#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class DirectoryWatcher {
 public:
    using Callback = std::function<void(const std::vector<std::string> &, bool fullRescan)>;
    DirectoryWatcher();
    ~DirectoryWatcher();
    bool start(const std::string &path, Callback callback, std::string &error,
               unsigned debounceMs = 3000, unsigned maximumDelayMs = 30000);
    void stop();
    bool running() const { return running_.load(); }
 private:
    void run();
    std::string path_; Callback callback_; unsigned debounceMs_=3000, maximumDelayMs_=30000;
    void *directoryHandle_=nullptr; std::atomic_bool running_{false}; std::thread worker_;
};

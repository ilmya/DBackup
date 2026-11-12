// 基于 Linux 文件系统事件的目录监控
// 在工作线程中监听 inotify 事件，处理目录变化、监控失效与重新扫描请求。
#include "automation/realtime/directory_watcher.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <poll.h>
#include <set>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// 创建未启动的监控器，线程将在调用 start 后建立。
DirectoryWatcher::DirectoryWatcher() = default;
// 销毁前停止监控线程，避免回调访问已释放对象。
DirectoryWatcher::~DirectoryWatcher() {
    stop();
}

// 初始化目录监控并启动工作线程，通过回调通知变化或错误
bool DirectoryWatcher::start(const std::string& path, Callback callback, std::string& error,
                             unsigned debounceMs, unsigned maximumDelayMs,
                             StatusCallback statusCallback, bool recursive) {
    stop();
    if (path.empty() || !callback) {
        error = "监控目录和事件回调不能为空";
        return false;
    }
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
        error = "实时监控来源必须是目录: " + path;
        return false;
    }
    path_ = path;
    callback_ = std::move(callback);
    statusCallback_ = std::move(statusCallback);
    debounceMs_ = debounceMs;
    maximumDelayMs_ = maximumDelayMs;
    recursive_ = recursive;
    error.clear();
    running_ = true;
    worker_ = std::thread(&DirectoryWatcher::run, this);
    return true;
}

// 请求监控线程退出，等待线程结束并释放资源
void DirectoryWatcher::stop() {
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

// 处理文件系统事件并维护受监控目录，将变化传递给回调
void DirectoryWatcher::run() {
    bool unavailable = false;
    while (running_) {
        std::error_code ec;
        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0 || !fs::is_directory(path_, ec)) {
            if (fd >= 0)
                close(fd);
            if (!unavailable && statusCallback_)
                statusCallback_(false, "监控目录不可用，将自动重试: " + path_);
            unavailable = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        std::map<int, fs::path> watches;
        auto add = [&](const fs::path& dir) {
            int wd =
                inotify_add_watch(fd, dir.c_str(),
                                  IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM |
                                      IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE);
            if (wd >= 0)
                watches[wd] = dir;
            return wd >= 0;
        };
        bool ok = add(path_);
        std::string partial;
        if (recursive_) {
            std::vector<fs::path> pending{path_};
            while (ok && !pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                std::error_code walkError;
                fs::directory_iterator it(current, walkError), end;
                if (walkError) {
                    if (partial.empty())
                        partial = current.string() + ": " + walkError.message();
                    continue;
                }
                for (; it != end; it.increment(walkError)) {
                    if (walkError)
                        break;
                    std::error_code typeError;
                    if (it->is_directory(typeError) && !it->is_symlink(typeError)) {
                        if (add(it->path()))
                            pending.push_back(it->path());
                        else if (partial.empty())
                            partial = it->path().string() + ": " + std::strerror(errno);
                    }
                    if (typeError && partial.empty())
                        partial = it->path().string() + ": " + typeError.message();
                }
                if (walkError && partial.empty())
                    partial = current.string() + ": " + walkError.message();
            }
        }
        if (!ok || ec) {
            close(fd);
            if (!unavailable && statusCallback_)
                statusCallback_(false, "无法监控目录，将自动重试: " + path_);
            unavailable = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (statusCallback_)
            statusCallback_(true, partial.empty()
                                      ? (unavailable ? "监控目录已恢复" : "监控目录运行正常")
                                      : "部分目录无法监控: " + partial);
        if (unavailable)
            callback_({}, true);
        unavailable = false;
        std::set<std::string> changes;
        Clock::time_point first, last;
        bool reconnect = false, rescan = false;
        alignas(inotify_event) char buffer[64 * 1024];
        while (running_ && !reconnect) {
            pollfd pfd{fd, POLLIN, 0};
            int ready = poll(&pfd, 1, 200);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                reconnect = true;
                break;
            }
            if (ready > 0 && (pfd.revents & POLLIN)) {
                ssize_t length = read(fd, buffer, sizeof(buffer));
                for (ssize_t offset = 0; offset < length;) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
                    if (event->mask & IN_Q_OVERFLOW) {
                        rescan = true;
                        reconnect = true;
                    }
                    auto found = watches.find(event->wd);
                    if (found != watches.end()) {
                        fs::path changed = found->second;
                        if (event->len)
                            changed /= event->name;
                        if (changes.empty())
                            first = Clock::now();
                        last = Clock::now();
                        changes.insert(changed.lexically_relative(path_).generic_string());
                        if (!partial.empty() && (event->mask & IN_ATTRIB)) {
                            reconnect = true;
                            rescan = true;
                        }
                        if (recursive_ && (event->mask & IN_ISDIR) &&
                            (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                            if (!add(changed)) {
                                if (statusCallback_)
                                    statusCallback_(true, "部分目录无法监控: " + changed.string() +
                                                              ": " + std::strerror(errno));
                            }
                            std::error_code walkError;
                            for (fs::recursive_directory_iterator
                                     it(changed, fs::directory_options::skip_permission_denied,
                                        walkError),
                                 end;
                                 !walkError && it != end; it.increment(walkError)) {
                                if (it->is_directory(walkError) && !it->is_symlink(walkError) &&
                                    !add(it->path())) {
                                    if (statusCallback_)
                                        statusCallback_(true,
                                                        "部分目录无法监控: " + it->path().string() +
                                                            ": " + std::strerror(errno));
                                }
                            }
                            if (walkError && statusCallback_)
                                statusCallback_(true, "部分目录无法监控: " + changed.string() +
                                                          ": " + walkError.message());
                            rescan = true;
                        }
                        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                            if (found->second == fs::path(path_))
                                reconnect = true;
                            watches.erase(event->wd);
                        }
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            if (rescan) {
                callback_({changes.begin(), changes.end()}, true);
                rescan = false;
                changes.clear();
            }
            if (!changes.empty()) {
                auto now = Clock::now();
                if (now - last >= std::chrono::milliseconds(debounceMs_) ||
                    (maximumDelayMs_ &&
                     now - first >= std::chrono::milliseconds(maximumDelayMs_))) {
                    callback_({changes.begin(), changes.end()}, false);
                    changes.clear();
                }
            }
        }
        close(fd);
        if (reconnect && running_) {
            unavailable = true;
            if (statusCallback_)
                statusCallback_(false, "监控目录已丢失，将自动重试: " + path_);
        }
    }
}

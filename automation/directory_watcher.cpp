#include "automation/directory_watcher.h"

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

DirectoryWatcher::DirectoryWatcher() = default;
DirectoryWatcher::~DirectoryWatcher() { stop(); }

bool DirectoryWatcher::start(const std::string &path, Callback callback, std::string &error,
                             unsigned debounceMs, unsigned maximumDelayMs, StatusCallback statusCallback) {
    stop();
    if (path.empty() || !callback) { error = "监控目录和事件回调不能为空"; return false; }
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::is_directory(path, ec)) { error = "实时监控来源必须是目录: " + path; return false; }
    path_ = path; callback_ = std::move(callback); statusCallback_ = std::move(statusCallback);
    debounceMs_ = debounceMs; maximumDelayMs_ = maximumDelayMs;
    error.clear(); running_ = true; worker_ = std::thread(&DirectoryWatcher::run, this); return true;
}

void DirectoryWatcher::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void DirectoryWatcher::run() {
    bool unavailable = false;
    while (running_) {
        std::error_code ec;
        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0 || !fs::is_directory(path_, ec)) {
            if (fd >= 0) close(fd);
            if (!unavailable && statusCallback_) statusCallback_(false, "监控目录不可用，将自动重试: " + path_);
            unavailable = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        std::map<int, fs::path> watches;
        auto add = [&](const fs::path &dir) {
            int wd = inotify_add_watch(fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE);
            if (wd >= 0) watches[wd] = dir;
            return wd >= 0;
        };
        bool ok = add(path_);
        for (fs::recursive_directory_iterator it(path_, fs::directory_options::skip_permission_denied, ec), end;
             ok && !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) ok = add(it->path());
        }
        if (!ok || ec) {
            close(fd);
            if (!unavailable && statusCallback_) statusCallback_(false, "无法监控目录，将自动重试: " + path_);
            unavailable = true; std::this_thread::sleep_for(std::chrono::milliseconds(250)); continue;
        }
        if (statusCallback_) statusCallback_(true, unavailable ? "监控目录已恢复" : "监控目录运行正常");
        if (unavailable) callback_({}, true);
        unavailable = false;
        std::set<std::string> changes;
        Clock::time_point first, last;
        bool reconnect = false, rescan = false;
        alignas(inotify_event) char buffer[64 * 1024];
        while (running_ && !reconnect) {
            pollfd pfd{fd, POLLIN, 0};
            int ready = poll(&pfd, 1, 200);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { reconnect = true; break; }
            if (ready > 0 && (pfd.revents & POLLIN)) {
                ssize_t length = read(fd, buffer, sizeof(buffer));
                for (ssize_t offset = 0; offset < length;) {
                    const auto *event = reinterpret_cast<const inotify_event *>(buffer + offset);
                    if (event->mask & IN_Q_OVERFLOW) { rescan = true; reconnect = true; }
                    auto found = watches.find(event->wd);
                    if (found != watches.end()) {
                        fs::path changed = found->second;
                        if (event->len) changed /= event->name;
                        if (changes.empty()) first = Clock::now();
                        last = Clock::now();
                        changes.insert(changed.lexically_relative(path_).generic_string());
                        if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                            if (!add(changed)) reconnect = true;
                            std::error_code walkError;
                            for (fs::recursive_directory_iterator it(changed, fs::directory_options::skip_permission_denied, walkError), end;
                                 !walkError && it != end; it.increment(walkError)) {
                                if (it->is_directory(walkError) && !it->is_symlink(walkError) && !add(it->path())) reconnect = true;
                            }
                            if (walkError) reconnect = true;
                            rescan = true;
                        }
                        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                            if (found->second == fs::path(path_)) reconnect = true;
                            watches.erase(event->wd);
                        }
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            if (rescan) { callback_({}, true); rescan = false; changes.clear(); }
            if (!changes.empty()) {
                auto now = Clock::now();
                if (now - last >= std::chrono::milliseconds(debounceMs_) ||
                    (maximumDelayMs_ && now - first >= std::chrono::milliseconds(maximumDelayMs_))) {
                    callback_({changes.begin(), changes.end()}, false); changes.clear();
                }
            }
        }
        close(fd);
        if (reconnect && running_) {
            unavailable = true;
            if (statusCallback_) statusCallback_(false, "监控目录已丢失，将自动重试: " + path_);
        }
    }
}

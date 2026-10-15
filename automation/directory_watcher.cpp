#include "automation/directory_watcher.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>

namespace {
constexpr DWORD kPollIntervalMs = 250;
constexpr DWORD kBufferSize = 64 * 1024;

std::wstring wide(const std::string &value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size) != size) return {};
    return result;
}

std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) return {};
    return result;
}

bool directoryExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void notifyStatus(const DirectoryWatcher::StatusCallback &callback, bool available,
                  const std::string &message) {
    if (!callback) return;
    try { callback(available, message); } catch (...) { }
}

void notifyChanges(const DirectoryWatcher::Callback &callback,
                   const std::set<std::string> &changes, bool fullRescan) {
    if (!callback) return;
    try { callback(std::vector<std::string>(changes.begin(), changes.end()), fullRescan); }
    catch (...) { }
}

bool collectChanges(const uint8_t *buffer, DWORD length, std::set<std::string> &changes) {
    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = static_cast<size_t>(length) - offset;
        if (remaining < offsetof(FILE_NOTIFY_INFORMATION, FileName)) return false;
        const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer + offset);
        const size_t nameBytes = info->FileNameLength;
        const size_t headerSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if ((nameBytes % sizeof(wchar_t)) != 0 || nameBytes > remaining - headerSize) return false;
        const std::wstring name(info->FileName, nameBytes / sizeof(wchar_t));
        const std::string encoded = utf8(name);
        if (encoded.empty() && !name.empty()) return false;
        changes.insert(encoded);
        if (info->NextEntryOffset == 0) return true;
        if (info->NextEntryOffset < headerSize + nameBytes || info->NextEntryOffset > remaining) return false;
        offset += info->NextEntryOffset;
    }
    return offset == length;
}
}

DirectoryWatcher::DirectoryWatcher() = default;

DirectoryWatcher::~DirectoryWatcher() { stop(); }

bool DirectoryWatcher::start(const std::string &path, Callback callback, std::string &error,
                             unsigned debounceMs, unsigned maximumDelayMs,
                             StatusCallback statusCallback) {
    stop();
    if (path.empty() || !callback) {
        error = "监控目录和事件回调不能为空";
        return false;
    }
    const std::wstring pathWide = wide(path);
    if (pathWide.empty()) {
        error = "监控目录路径不是有效的 UTF-8";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(pathWide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "实时监控来源必须是目录: " + path;
        return false;
    }

    path_ = path;
    callback_ = std::move(callback);
    statusCallback_ = std::move(statusCallback);
    debounceMs_ = debounceMs;
    maximumDelayMs_ = maximumDelayMs;
    error.clear();
    running_ = true;
    worker_ = std::thread(&DirectoryWatcher::run, this);
    return true;
}

void DirectoryWatcher::stop() {
    if (!running_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    if (worker_.joinable()) worker_.join();
}

void DirectoryWatcher::run() {
    const std::wstring pathWide = wide(path_);
    bool unavailableReported = false;
    std::vector<uint8_t> buffer(kBufferSize);

    while (running_) {
        if (!directoryExists(pathWide)) {
            if (!unavailableReported) {
                notifyStatus(statusCallback_, false, "监控目录不可用，将自动重试: " + path_);
                unavailableReported = true;
            }
            for (DWORD waited = 0; running_ && waited < 1000; waited += kPollIntervalMs)
                Sleep(kPollIntervalMs);
            continue;
        }

        HANDLE directory = CreateFileW(pathWide.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            if (!unavailableReported) {
                notifyStatus(statusCallback_, false, "无法打开监控目录，将自动重试: " + path_);
                unavailableReported = true;
            }
            for (DWORD waited = 0; running_ && waited < 1000; waited += kPollIntervalMs)
                Sleep(kPollIntervalMs);
            continue;
        }

        const bool recovered = unavailableReported;
        unavailableReported = false;
        notifyStatus(statusCallback_, true, recovered ? "监控目录已恢复" : "监控目录运行正常");
        if (recovered) notifyChanges(callback_, {}, true);

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            notifyStatus(statusCallback_, false, "无法创建目录监控事件，将自动重试");
            CloseHandle(directory);
            unavailableReported = true;
            Sleep(kPollIntervalMs);
            continue;
        }

        std::set<std::string> changes;
        auto firstChange = std::chrono::steady_clock::time_point{};
        auto lastChange = std::chrono::steady_clock::time_point{};
        bool reconnect = false;

        while (running_ && !reconnect) {
            ResetEvent(event);
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            DWORD bytes = 0;
            const BOOL issued = ReadDirectoryChangesW(directory, buffer.data(),
                static_cast<DWORD>(buffer.size()), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_CREATION,
                nullptr, &overlapped, nullptr);
            DWORD completionError = issued ? ERROR_SUCCESS : GetLastError();
            bool completed = false;
            if (issued) {
                if (GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
                    completed = true;
                } else {
                    completionError = GetLastError();
                    completed = completionError != ERROR_IO_INCOMPLETE;
                }
            } else if (completionError != ERROR_IO_PENDING) {
                completed = true;
            }
            bool rootLost = false;

            while (running_ && !completed && !rootLost) {
                DWORD waitMs = kPollIntervalMs;
                if (!changes.empty()) {
                    const auto now = std::chrono::steady_clock::now();
                    auto due = lastChange + std::chrono::milliseconds(debounceMs_);
                    if (maximumDelayMs_ > 0) {
                        due = std::min(due, firstChange + std::chrono::milliseconds(maximumDelayMs_));
                    }
                    if (debounceMs_ == 0 || due <= now) {
                        notifyChanges(callback_, changes, false);
                        changes.clear();
                        continue;
                    }
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count();
                    waitMs = static_cast<DWORD>(std::min<int64_t>(waitMs, std::max<int64_t>(1, remaining)));
                }

                const DWORD wait = WaitForSingleObject(event, waitMs);
                if (wait == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
                        completed = true;
                        completionError = ERROR_SUCCESS;
                    } else {
                        completionError = GetLastError();
                        completed = completionError != ERROR_IO_INCOMPLETE;
                    }
                    continue;
                }
                if (wait == WAIT_FAILED) {
                    completionError = GetLastError();
                    completed = true;
                    continue;
                }
                if (!directoryExists(pathWide)) {
                    rootLost = true;
                    CancelIoEx(directory, &overlapped);
                    WaitForSingleObject(event, INFINITE);
                    break;
                }
            }

            if (!running_) {
                if (!completed) {
                    CancelIoEx(directory, &overlapped);
                    WaitForSingleObject(event, INFINITE);
                }
                break;
            }
            if (rootLost) {
                notifyStatus(statusCallback_, false, "监控目录已丢失，将自动重试: " + path_);
                unavailableReported = true;
                reconnect = true;
                break;
            }
            if (!completed) continue;
            if (completionError == ERROR_NOTIFY_ENUM_DIR || (completionError == ERROR_SUCCESS && bytes == 0)) {
                changes.clear();
                notifyChanges(callback_, {}, true);
                continue;
            }
            if (completionError == ERROR_OPERATION_ABORTED && !running_) break;
            if (completionError != ERROR_SUCCESS) {
                notifyStatus(statusCallback_, false, "目录监控暂时中断，将自动重试: " + path_);
                unavailableReported = true;
                reconnect = true;
                break;
            }
            const bool hadChanges = !changes.empty();
            if (bytes > buffer.size() || !collectChanges(buffer.data(), bytes, changes)) {
                changes.clear();
                notifyChanges(callback_, {}, true);
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!hadChanges) firstChange = now;
            lastChange = now;
            if (debounceMs_ == 0) {
                notifyChanges(callback_, changes, false);
                changes.clear();
            }
        }

        if (reconnect) changes.clear();
        if (!changes.empty() && running_) notifyChanges(callback_, changes, false);
        CloseHandle(event);
        CloseHandle(directory);
    }
}

#include "core/archive.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
// 按当前区域设置外的 ASCII 规则归一化筛选文本大小写。
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
} // namespace

bool ParseFilterOptions(const std::string& spec, FilterOptions& filter, std::string& error) {
    filter = FilterOptions{};
    error.clear();
    auto trim = [](std::string value) {
        const char* spaces = " \t\r\n";
        size_t first = value.find_first_not_of(spaces);
        if (first == std::string::npos)
            return std::string{};
        size_t last = value.find_last_not_of(spaces);
        return value.substr(first, last - first + 1);
    };
    auto parseSize = [&](const std::string& value, uint64_t& target, const std::string& key) {
        if (value.empty() || value[0] == '-') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        errno = 0;
        char* end = nullptr;
        unsigned long long number = std::strtoull(value.c_str(), &end, 10);
        if (errno == ERANGE || end == value.c_str() || *end != '\0') {
            error = "筛选项 " + key + " 必须是非负整数";
            return false;
        }
        target = static_cast<uint64_t>(number);
        return true;
    };
    auto parseDate = [&](const std::string& value, int64_t& target, const std::string& key,
                         bool endOfDay) {
        std::tm tm = {};
        std::istringstream input(value);
        input >> std::get_time(&tm, "%Y-%m-%d");
        if (input.fail() || input.peek() != std::char_traits<char>::eof()) {
            error = "筛选项 " + key + " 的日期格式应为 YYYY-MM-DD";
            return false;
        }
        const int expectedYear = tm.tm_year;
        const int expectedMonth = tm.tm_mon;
        const int expectedDay = tm.tm_mday;
        time_t seconds = timegm(&tm);
        if (seconds == static_cast<time_t>(-1)) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        std::tm check = {};
        gmtime_r(&seconds, &check);
        if (check.tm_year != expectedYear || check.tm_mon != expectedMonth ||
            check.tm_mday != expectedDay) {
            error = "筛选项 " + key + " 的日期无效";
            return false;
        }
        target = static_cast<int64_t>(seconds) * 1000 + (endOfDay ? 86399999LL : 0LL);
        return true;
    };

    std::stringstream stream(spec);
    std::string part;
    while (std::getline(stream, part, ';')) {
        part = trim(part);
        if (part.empty())
            continue;
        size_t equal = part.find('=');
        if (equal == std::string::npos) {
            error = "筛选项缺少等号: " + part;
            return false;
        }
        std::string key = Lower(trim(part.substr(0, equal)));
        std::string value = trim(part.substr(equal + 1));
        if (key.empty() || value.empty()) {
            error = "筛选项的名称和值不能为空: " + part;
            return false;
        }
        if (key == "path")
            filter.pathContains = value;
        else if (key == "name")
            filter.nameContains = value;
        else if (key == "ext" || key == "extension") {
            std::stringstream extensions(value);
            std::string extension;
            while (std::getline(extensions, extension, ',')) {
                extension = trim(extension);
                if (!extension.empty())
                    filter.extensions.push_back(extension);
            }
            if (filter.extensions.empty()) {
                error = "筛选项 ext 至少需要一个扩展名";
                return false;
            }
            if (filter.extensions.size() == 1)
                filter.extension = filter.extensions.front();
        } else if (key == "include" || key == "exclude") {
            filter.pathRules.push_back(PathFilterRule{key == "include", value});
        } else if (key == "owner" || key == "user")
            filter.ownerContains = value;
        else if (key == "type") {
            std::string type = Lower(value);
            if (type == "file" || type == "files")
                filter.type = EntryTypeFilter::FilesOnly;
            else if (type == "dir" || type == "directory" || type == "directories") {
                filter.type = EntryTypeFilter::DirectoriesOnly;
            } else {
                error = "筛选项 type 只支持 file 或 dir";
                return false;
            }
        } else if (key == "minsize") {
            if (!parseSize(value, filter.minSize, key))
                return false;
        } else if (key == "maxsize") {
            if (!parseSize(value, filter.maxSize, key))
                return false;
        } else if (key == "after") {
            if (!parseDate(value, filter.afterMtimeMs, key, false))
                return false;
        } else if (key == "before") {
            if (!parseDate(value, filter.beforeMtimeMs, key, true))
                return false;
        } else {
            error = "未知筛选项: " + key;
            return false;
        }
    }
    if (filter.minSize != 0 && filter.maxSize != 0 && filter.minSize > filter.maxSize) {
        error = "minsize 不能大于 maxsize";
        return false;
    }
    if (filter.afterMtimeMs != 0 && filter.beforeMtimeMs != 0 &&
        filter.afterMtimeMs > filter.beforeMtimeMs) {
        error = "after 不能晚于 before";
        return false;
    }
    return true;
}


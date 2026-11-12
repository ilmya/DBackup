// 验证support相关功能
#pragma once

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <stdlib.h>

namespace test_support {

// 为每个文件系统用例创建独立临时目录，并在用例结束时自动清理
class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::string prefix = "dbackup-test") {
        std::string pattern =
            (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
        std::vector<char> writablePattern(pattern.begin(), pattern.end());
        writablePattern.push_back('\0');

        char* createdPath = mkdtemp(writablePattern.data());
        if (!createdPath) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
        }
        path_ = createdPath;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }
    std::string string() const {
        return path_.string();
    }

  private:
    std::filesystem::path path_;
};

// 写入小型文本夹具，统一使用二进制模式避免平台换行转换
inline void writeText(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// 读取文本夹具的全部字节，便于比较备份恢复前后的内容
inline std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace test_support

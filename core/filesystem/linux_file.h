// 封装 Linux 文件元数据与安全目录操作
// 定义文件元数据结构和安全路径、目录及元数据处理函数，供扫描与恢复共享。
// 读取对象类型、属主、权限和时间，并通过目录描述符避免不安全的路径遍历
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dbackup::linux_file {

// 保存 Linux 文件的权限、属主、时间、标识和设备等元数据
struct Metadata {
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    int64_t mtimeNs = 0;
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    uint64_t links = 0;
    uint32_t deviceMajor = 0;
    uint32_t deviceMinor = 0;
};

// 使用 lstat 读取对象自身的 POSIX 类型、属主、权限、时间和链接信息
bool readMetadata(const std::filesystem::path& path, Metadata& metadata, std::string& error);
// 仅接受非绝对且不包含父目录跳转的备份相对路径
bool validRelativePath(const std::filesystem::path& path);
// 安全打开目录描述符；成功后由调用方关闭，失败返回 -1
int openDirectory(const std::filesystem::path& path, bool create, std::string& error);
// 逐级创建目录并拒绝经过符号链接穿越目标边界
bool ensureDirectories(const std::filesystem::path& path, std::string& error);

} // namespace dbackup::linux_file

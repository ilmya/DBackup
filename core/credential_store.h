#pragma once
#include <string>

class CredentialStore {
  public:
    // 将密码交给桌面密钥环保存，不写入计划配置文件。
    static bool save(const std::string& name, const std::string& secret, std::string& error);
    // 从桌面密钥环读取密码；密钥环不可用或条目不存在时返回原因。
    static bool load(const std::string& name, std::string& secret, std::string& error);
    // 从桌面密钥环删除指定密码条目。
    static bool remove(const std::string& name, std::string& error);
};

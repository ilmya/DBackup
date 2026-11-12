// 封装系统密钥环的备份凭据存取
// 定义系统密钥环访问接口，避免把计划密码写入普通配置文件。
// 使用 Secret Service 保存、读取和删除指定名称的密码，并报告服务不可用等错误
#pragma once
#include <string>

// 通过系统密钥环保存、读取和删除命名凭据
class CredentialStore {
  public:
    // 将密码交给桌面密钥环保存，不写入计划配置文件
    static bool save(const std::string& name, const std::string& secret, std::string& error);
    // 从桌面密钥环读取密码；密钥环不可用或条目不存在时返回原因
    static bool load(const std::string& name, std::string& secret, std::string& error);
    // 从桌面密钥环删除指定密码条目
    static bool remove(const std::string& name, std::string& error);
};

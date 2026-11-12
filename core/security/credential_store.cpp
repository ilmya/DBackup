// 封装系统密钥环的备份凭据存取
// 通过 Secret Service 按名称保存、读取和删除密钥，将系统服务错误传递给调用方。
// 使用 Secret Service 保存、读取和删除指定名称的密码，并报告服务不可用等错误
#include "core/security/credential_store.h"
#include <libsecret/secret.h>

namespace {
const SecretSchema schema = {
    "org.dbackup.Credential",
    SECRET_SCHEMA_NONE,
    {{"name", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

// 将 libsecret 错误对象转换为应用可展示的错误文本
bool report(GError* failure, std::string& error) {
    if (!failure)
        return true;
    error = failure->message;
    g_error_free(failure);
    return false;
}
} // namespace

// 将指定名称的凭据保存到系统密钥环
bool CredentialStore::save(const std::string& name, const std::string& secret, std::string& error) {
    GError* failure = nullptr;
    gboolean ok =
        secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "DBackup", secret.c_str(),
                                   nullptr, &failure, "name", name.c_str(), nullptr);
    if (!report(failure, error))
        return false;
    if (!ok)
        error = "无法保存凭据";
    return ok;
}

// 从系统密钥环读取指定名称的凭据
bool CredentialStore::load(const std::string& name, std::string& secret, std::string& error) {
    GError* failure = nullptr;
    gchar* value =
        secret_password_lookup_sync(&schema, nullptr, &failure, "name", name.c_str(), nullptr);
    if (!report(failure, error))
        return false;
    if (!value) {
        error = "凭据不存在";
        return false;
    }
    secret = value;
    secret_password_free(value);
    return true;
}

// 从系统密钥环删除指定名称的凭据
bool CredentialStore::remove(const std::string& name, std::string& error) {
    GError* failure = nullptr;
    secret_password_clear_sync(&schema, nullptr, &failure, "name", name.c_str(), nullptr);
    return report(failure, error);
}

#include "core/credential_store.h"
#include <libsecret/secret.h>

namespace {
const SecretSchema schema = {
    "org.dbackup.Credential",
    SECRET_SCHEMA_NONE,
    {{"name", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

// 将 libsecret 错误对象转换为应用可展示的错误文本。
bool report(GError* failure, std::string& error) {
    if (!failure)
        return true;
    error = failure->message;
    g_error_free(failure);
    return false;
}
} // namespace

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

bool CredentialStore::remove(const std::string& name, std::string& error) {
    GError* failure = nullptr;
    secret_password_clear_sync(&schema, nullptr, &failure, "name", name.c_str(), nullptr);
    return report(failure, error);
}

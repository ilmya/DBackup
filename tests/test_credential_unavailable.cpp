// 验证密钥环不可用时的凭据错误处理
#include "core/security/credential_store.h"
#include <gtest/gtest.h>

TEST(CredentialUnavailable, ReportsSaveAndLoadFailures) {
    std::string error;
    EXPECT_FALSE(CredentialStore::save("isolated-test", "test-only-password", error));
    EXPECT_FALSE(error.empty());

    std::string secret;
    error.clear();
    EXPECT_FALSE(CredentialStore::load("isolated-test", secret, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(secret.empty());
}

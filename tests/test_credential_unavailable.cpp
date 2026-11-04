#include "core/credential_store.h"
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

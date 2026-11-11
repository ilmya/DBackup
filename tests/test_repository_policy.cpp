#include "core/repository.h"
#include <gtest/gtest.h>
TEST(RepositoryPolicy, EmptyHistory) { EXPECT_EQ(dbackup::repository_policy::retainedCount(0, 5), 0u); }
TEST(RepositoryPolicy, KeepNone) { EXPECT_EQ(dbackup::repository_policy::retainedCount(3, 0), 0u); }
TEST(RepositoryPolicy, KeepNewest) { EXPECT_EQ(dbackup::repository_policy::retainedCount(3, 1), 1u); }
TEST(RepositoryPolicy, KeepAll) { EXPECT_EQ(dbackup::repository_policy::retainedCount(3, 3), 3u); }
TEST(RepositoryPolicy, KeepMoreThanHistory) { EXPECT_EQ(dbackup::repository_policy::retainedCount(3, 10), 3u); }
